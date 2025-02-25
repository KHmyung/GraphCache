/*
 * Copyright 2014 Open Connectome Project (http://openconnecto.me)
 * Written by Da Zheng (zhengda1936@gmail.com)
 *
 * This file is part of SAFSlib.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <errno.h>
#include <limits.h>
#include <mutex>
#include <algorithm>


#include "io_interface.h"
#include "associative_cache.h"
#include "dirty_page_flusher.h"
#include "safs_exception.h"
#include "memory_manager.h"



namespace safs
{

#define CLOCK 0
#define LIFO 1
#define DEFAULT_POLICY LIFO

// APR DEBUG CODE

//#define APR_DEBUG_PRINT
#define APR_DEBUG_TIME
//#define APR_DEBUG_TIME_DETAIL
#define APR_DEBUG_STAT
#define MILLION (1000000ULL)
std::mutex print_lock;
ghost_t *global_ghost;
page_info_t *page_info;

int *access_log;

std::atomic<int> time_check;
std::atomic<double> global_score;
std::atomic<bool> global_policy;

struct TimeFormat cache_time;

static unsigned long long * __time_alloc(int nr){
	return (unsigned long long *)calloc(nr, sizeof(unsigned long long));
}

static void time_init(int nr_thread, struct TimeFormat *time){
	time->total_t = 0;
	time->total_c = 0;
	time->pol_clock_t = __time_alloc(nr_thread);
	time->pol_clock_c = __time_alloc(nr_thread);
	time->pol_lifo_t = __time_alloc(nr_thread);
	time->pol_lifo_c = __time_alloc(nr_thread);
	time->pol_compete_t = __time_alloc(nr_thread);
	time->pol_compete_c = __time_alloc(nr_thread);
}

const long default_init_cache_size = 128 * 1024 * 1024;

template<class T>
void page_cell<T>::set_pages(char *pages[], int num, int node_id)
{
	assert(num <= CELL_SIZE);
	page_id_t pg_id;
	// KH: initialize pages
	for (int i = 0; i < num; i++) {
		buf[i] = T(pg_id, pages[i], node_id);
	}
	idx = 0;
	num_pages = num;
	for (int i = 0; i < num; i++) {
		maps[i] = i;
	}
}

template<class T>
void page_cell<T>::rebuild_map()
{
	int j = 0;
	for (int i = 0; i < CELL_SIZE; i++) {
		if (buf[i].get_data())
			maps[j++] = i;
	}
	assert(j == num_pages);
}

template<class T>
void page_cell<T>::add_pages(char *pages[], int num, int node_id)
{
	int num_added = 0;
	assert(num_pages == get_num_used_pages());
	assert(num + num_pages <= CELL_SIZE);
	page_id_t pg_id;
	for (int i = 0; i < CELL_SIZE && num_added < num; i++) {
		if (buf[i].get_data() == NULL)
			buf[i] = T(pg_id, pages[num_added++], node_id);
	}
	num_pages += num;
	rebuild_map();
}

template<class T>
void page_cell<T>::inject_pages(T pages[], int npages)
{
	int num_copied = 0;
	for (int i = 0; i < CELL_SIZE && num_copied < npages; i++) {
		if (buf[i].get_data() == NULL) {
			buf[i] = pages[num_copied++];
		}
	}
	assert(num_copied == npages);
	num_pages += num_copied;
	rebuild_map();
}

template<class T>
void page_cell<T>::steal_pages(T pages[], int &npages)
{
	int num_copied = 0;
	for (int i = 0; i < CELL_SIZE && num_copied < npages; i++) {
		if (buf[i].get_data()) {
			// We have to make sure the page isn't being referenced.
			// TODO busy wait.
			while (buf[i].get_ref() > 0) {}
			pages[num_copied++] = buf[i];
			buf[i] = T();
		}
	}
	npages = num_copied;
	num_pages -= num_copied;
	if (num_pages > 0)
		rebuild_map();
	else
		memset(maps, 0, sizeof(maps));
}

template<class T>
void page_cell<T>::sanity_check() const
{
	assert(params.get_SA_min_cell_size() <= num_pages);
	int num_used_pages = 0;
	for (int i = 0; i < CELL_SIZE; i++)
		if (buf[i].get_data())
			num_used_pages++;
	assert(num_used_pages == num_pages);
	int prev_map = -1;
	for (int i = 0; i < num_pages; i++) {
		int map = maps[i];
		if (prev_map >= 0)
			assert(map > prev_map);
		assert(buf[map].get_data());
		prev_map = map;
	}
}

template<class T>
int page_cell<T>::get_num_used_pages() const
{
	int num = 0;
	for (int i = 0; i < CELL_SIZE; i++)
		if (buf[i].get_data())
			num++;
	return num;
}

void hash_cell::init(bool sample){
#ifdef USE_APR
	if (sample)
		policy.notify_sample();
#endif
}

void hash_cell::init(associative_cache *cache, long hash, bool get_pages) {
	this->hash = hash;
	assert(hash < INT_MAX);
	this->table = cache;
	if (get_pages) {
		char *pages[CELL_SIZE];
		if (!table->get_manager()->get_free_pages(params.get_SA_min_cell_size(),
					pages, cache))
			throw oom_exception();
		buf.set_pages(pages, params.get_SA_min_cell_size(), table->get_node_id());
	}
	num_accesses = 0;
	num_evictions = 0;
}

void hash_cell::sanity_check()
{
	_lock.lock();
	buf.sanity_check();
	assert(!is_referenced());
	_lock.unlock();
}

void hash_cell::add_pages(char *pages[], int num)
{
	buf.add_pages(pages, num, table->get_node_id());
}

int hash_cell::add_pages_to_min(char *pages[], int num)
{
	int num_required = CELL_MIN_NUM_PAGES - buf.get_num_pages();
	if (num_required > 0) {
		num_required = min(num_required, num);
		buf.add_pages(pages, num_required, table->get_node_id());
		return num_required;
	}
	else
		return 0;
}

void hash_cell::merge(hash_cell *cell)
{
	_lock.lock();
	cell->_lock.lock();

	assert(cell->get_num_pages() + this->get_num_pages() <= CELL_SIZE);
	thread_safe_page pages[CELL_SIZE];
	int npages = CELL_SIZE;
	// TODO there may be busy wait in this method.
	cell->buf.steal_pages(pages, npages);
	buf.inject_pages(pages, npages);

	cell->_lock.unlock();
	_lock.unlock();
}

/**
 * rehash the pages in the current cell to the expanded cell.
 */
void hash_cell::rehash(hash_cell *expanded)
{
	_lock.lock();
	expanded->_lock.lock();
	thread_safe_page *exchanged_pages_pointers[CELL_SIZE];
	int num_exchanges = 0;
	for (unsigned int i = 0; i < buf.get_num_pages(); i++) {
		thread_safe_page *pg = buf.get_page(i);
		page_id_t pg_id(pg->get_file_id(), pg->get_offset());
		int hash1 = table->hash1_locked(pg_id);
		/*
		 * It's possible that a page is in a wrong cell.
		 * It's likely because the page is added to the cell
		 * right when `level' is increased.
		 * But the case is rare, so we can just simple ignore
		 * the case. It doesn't affect the correctness of
		 * the implementation. The only penalty is that
		 * Since the page is in a wrong cell, it won't be
		 * accessed any more, so we should shorten the time
		 * it gets evicted by setting its hit to 1.
		 */
		if (hash1 != expanded->hash) {
			pg->set_hits(1);
			continue;
		}
		/*
		 * if the two hash values don't match,
		 * it means the page is mapped to the expanded cell.
		 * we exchange the pages in the two cells.
		 */
		if (this->hash != hash1) {
			/*
			 * we have to make sure no other threads are using them
			 * before we can exchange them.
			 * If the pages are in use, skip them.
			 */
			if (pg->get_ref())
				continue;

			exchanged_pages_pointers[num_exchanges++] = pg;
			// We can't steal pages while iterating them.
		}
	}
	if (num_exchanges > 0) {
		// We can only steal pages here.
		thread_safe_page exchanged_pages[CELL_SIZE];
		for (int i = 0; i < num_exchanges; i++) {
			exchanged_pages[i] = *exchanged_pages_pointers[i];
			buf.steal_page(exchanged_pages_pointers[i], false);
			*exchanged_pages_pointers[i] = thread_safe_page();
		}
		buf.rebuild_map();
		expanded->buf.inject_pages(exchanged_pages, num_exchanges);
	}

	// Move empty pages to the expanded cell if it doesn't have enough pages.
	int num_required = params.get_SA_min_cell_size() - expanded->buf.get_num_pages();
	int num_empty = 0;
	if (num_required > 0) {
		thread_safe_page *empty_pages_pointers[num_required];
		thread_safe_page *empty_pages = new thread_safe_page[params.get_SA_min_cell_size()];
		for (unsigned int i = 0; i < buf.get_num_pages()
				&& num_empty < num_required; i++) {
			thread_safe_page *pg = buf.get_page(i);
			if (!pg->initialized())
				empty_pages_pointers[num_empty++] = pg;
		}
		for (int i = 0; i < num_empty; i++) {
			// For the same reason, we can't steal pages
			// while iterating them.
			empty_pages[i] = *empty_pages_pointers[i];
			buf.steal_page(empty_pages_pointers[i], false);
			*empty_pages_pointers[i] = thread_safe_page();
		}
		buf.rebuild_map();
		expanded->buf.inject_pages(empty_pages, num_empty);
		delete [] empty_pages;
	}
	expanded->_lock.unlock();
	_lock.unlock();
}

void hash_cell::steal_pages(char *pages[], int &npages)
{
	int num_stolen = 0;
	while (num_stolen < npages) {
		thread_safe_page *pg = get_empty_page();
		if (pg == NULL)
			break;
		assert(!pg->is_dirty());
		pages[num_stolen++] = (char *) pg->get_data();
		*pg = thread_safe_page();
		buf.steal_page(pg, false);
	}
	buf.rebuild_map();
	npages = num_stolen;
}

void hash_cell::rebalance(hash_cell *cell)
{
	// TODO
}

page *hash_cell::search(const page_id_t &pg_id)
{
	_lock.lock();
	page *ret = NULL;
	for (unsigned int i = 0; i < buf.get_num_pages(); i++) {
		if (buf.get_page(i)->get_offset() == pg_id.get_offset()
				&& buf.get_page(i)->get_file_id() == pg_id.get_file_id()) {
			ret = buf.get_page(i);
			break;
		}
	}
	if (ret)
		ret->inc_ref();
	_lock.unlock();
	return ret;
}

/**
 * search for a page with the offset.
 * If the page doesn't exist, return an empty page.
 */
page *hash_cell::search(const page_id_t &pg_id, page_id_t &old_id)
{
	thread_safe_page *ret = NULL;
	_lock.lock();
	policy.update_hash(hash);
	pthread_t th_id;
	th_id = pthread_self();
	num_accesses++;
#ifdef APR_DEBUG_STAT
	//access_log[pg_id.get_offset()/PAGE_SIZE]++;
	APR_stats.cache_access++;
#endif

	for (unsigned int i = 0; i < buf.get_num_pages(); i++) {
		// KH: cache hit
		if (buf.get_page(i)->get_offset() == pg_id.get_offset()
				&& buf.get_page(i)->get_file_id() == pg_id.get_file_id()) {
			ret = buf.get_page(i);
#ifdef APR_DEBUG_PRINT
			if (policy.check_sample()){
				std::cout << "\nHashcell: " << hash << " (Thread " << th_id << ")" << std::endl;
				std::cout << "CACHE HIT: " << pg_id.get_offset()/PAGE_SIZE << std::endl;
			}
#endif
			if (th_id != ret->get_boundary_id())
				ret->clear_boundary();
			APR_stats.hit_count++;
			break;
		}
	}
	if (ret == NULL) {
		num_evictions++;
#ifdef USE_APR
		ret = get_empty_page(pg_id.get_offset()/PAGE_SIZE);	// arrange a victim page to reclaim
#else
		ret = get_empty_page();
#endif
		if (ret == NULL) {
			_lock.unlock();
			return NULL;
		}

		if (page_info[pg_id.get_offset()/PAGE_SIZE].boundary){
#ifdef APR_DEBUG_PRINT
			if (policy.check_sample()){
				std::cout << "boundary set : " << pg_id.get_offset()/PAGE_SIZE << std::endl;
			}
#endif
			ret->set_boundary(th_id);
		}

#ifdef APR_DEBUG_STAT
		APR_stats.miss_count++;
		//print_lock.lock();
		//std::cout << pg_id.get_offset() << std::endl;
		//print_lock.unlock();
		if (ret->get_pg_offset() == -1)
			APR_stats.cold_miss++;
#endif
		// We need to clear flags here.
		ret->set_data_ready(false);
		assert(!ret->is_io_pending());
		// We don't clear the prepare writeback flag because this flag
		// indicates that the page is in the queue for writing back, so
		// the flusher doesn't need to add another request to flush the
		// page. The flag will be cleared after it is removed from the queue.

		if (ret->is_dirty() && !ret->is_old_dirty()) {
			ret->set_dirty(false);
			ret->set_old_dirty(true);
		}
		// KH: offset of victim
		off_t old_off = ret->get_offset();
		file_id_t old_file_id = ret->get_file_id();
		if (old_off == -1) {
			old_off = PAGE_INVALID_OFFSET;
			assert(old_file_id == INVALID_FILE_ID);
		}
		// KH: page_id of victim
		old_id = page_id_t(old_file_id, old_off);
		/*
		 * I have to change the offset in the spinlock,
		 * to make sure when the spinlock is unlocked,
		 * the page can be seen by others even though
		 * it might not have data ready.
		 */
		// KH: allocate the victim page to new page id
		ret->set_id(pg_id);
#ifdef USE_SHADOW_PAGE
		shadow_page shadow_pg = shadow.search(off);
		/*
		 * if the page has been seen before,
		 * we should set the hits info.
		 */
		if (shadow_pg.is_valid())
			ret->set_hits(shadow_pg.get_hits());
#endif
	}
	else
		policy.access_page(ret, buf);
	/* it's possible that the data in the page isn't ready */
	ret->inc_ref();
	if (ret->get_hits() == 0xff) {
		buf.scale_down_hits();
#ifdef USE_SHADOW_PAGE
		shadow.scale_down_hits();
#endif
	}
	ret->hit();
	_lock.unlock();
#ifdef DEBUG
	if (enable_debug && ret->is_old_dirty())
		print_cell();
#endif
	return ret;
}

void hash_cell::print_cell()
{
	_lock.lock();
	printf("cell %ld: in queue: %d\n", get_hash(), is_in_queue());
	for (unsigned int i = 0; i < buf.get_num_pages(); i++) {
		thread_safe_page *p = buf.get_page(i);
		printf("cell %ld: p%lx, hits: %d, score: %d, ref: %d, r: %d, p: %d, d: %d, od: %d, w: %d\n",
				get_hash(), p->get_offset(), p->get_hits(), p->get_flush_score(),
				p->get_ref(), p->data_ready(), p->is_io_pending(), p->is_dirty(),
				p->is_old_dirty(), p->is_prepare_writeback());
	}
	_lock.unlock();
}


// KH: get_empty_page in APR
thread_safe_page *hash_cell::get_empty_page(off_t offset)
{

	struct timespec local_time[2];
#ifdef APR_DEBUG_TIME
	clock_gettime(CLOCK_MONOTONIC, &local_time[0]);
#endif

#ifdef USE_APR
	thread_safe_page *ret = policy.evict_page(buf, offset);
#else
	thread_safe_page *ret = policy.evict_page(buf);
#endif
	if (ret == NULL) {
#ifdef APR_DEBUG_PRINT
		printf("[apr] all pages in the cell were all referenced\n");
#endif
		return NULL;
	}

#ifdef APR_DEBUG_TIME
	clock_gettime(CLOCK_MONOTONIC, &local_time[1]);
	calclock(local_time, &cache_time.total_t, &cache_time.total_c);
#endif

	/* we record the hit info of the page in the shadow cell. */

	return ret;
}

/* this function has to be called with lock held */
thread_safe_page *hash_cell::get_empty_page()
{

	struct timespec local_time[2];
#ifdef APR_DEBUG_TIME
	clock_gettime(CLOCK_MONOTONIC, &local_time[0]);
#endif

	thread_safe_page *ret = policy.evict_page(buf);
	if (ret == NULL) {
#ifdef APR_DEBUG_PRINT
		printf("[clock/lifo] all pages in the cell were all referenced\n");
#endif
		return NULL;
	}

#ifdef APR_DEBUG_TIME
	clock_gettime(CLOCK_MONOTONIC, &local_time[1]);
	calclock(local_time, &cache_time.total_t, &cache_time.total_c);
#endif

#ifdef USE_SHADOW_PAGE
	if (ret->get_hits() > 0)
		shadow.add(shadow_page(*ret));
#endif

	return ret;
}


void LIFO_eviction_policy::init()
{
	num_entry = 0;
	lifo_head = 0;
	warm_up = true;

	for (int i = 0; i < params.get_SA_min_cell_size(); i++){
		lifo_lt.push_back(i);
	}
}

thread_safe_page *LIFO_eviction_policy::evict_page(
		page_cell<thread_safe_page> &buf)
{
	thread_safe_page *ret = NULL;
	unsigned int num_dirty = 0;
	unsigned int num_referenced = 0;
	bool avoid_dirty = true;

	std::list<int>::iterator lifo_iter = lifo_lt.end();
	lifo_iter--;

	if (check_warm_up()) {
		thread_safe_page *pg = buf.get_page(lifo_head % buf.get_num_pages());
		lifo_head++;
		num_entry++;
		ret = pg;
		if (buf.get_num_pages() <= get_num_entry()){
			finish_warm_up();
		}
	}
	else do {
		int lifo_offset = *lifo_iter;
		lifo_iter--;

		if (lifo_offset == buf.get_num_pages())
			continue;

		thread_safe_page *pg = buf.get_page(lifo_offset);

		if (num_referenced + num_dirty >= buf.get_num_pages()) {
			num_referenced = 0;
			num_dirty = 0;
			avoid_dirty = false;
		}
		if (pg->get_ref()) {
			num_referenced++;
			if (num_referenced >= buf.get_num_pages())
				return NULL;
			continue;
		}
		if (avoid_dirty && pg->is_dirty()) {
			num_dirty++;
			continue;
		}
		ret = pg;
		lifo_head = lifo_offset;
		lifo_lt.remove(lifo_head);
		lifo_lt.push_back(lifo_head);
	} while (ret == NULL);
	ret->set_data_ready(false);

	return ret;
}
/*
 * the end of the vector points to the pages
 * that are most recently accessed.
 */
thread_safe_page *LRU_eviction_policy::evict_page(
		page_cell<thread_safe_page> &buf)
{
	int pos;
	if (pos_vec.size() < buf.get_num_pages()) {
		pos = pos_vec.size();
	}
	else {
		/* evict the first page */
		pos = pos_vec[0];
		pos_vec.erase(pos_vec.begin());
	}
	thread_safe_page *ret = buf.get_page(pos);
	while (ret->get_ref()) {}
	pos_vec.push_back(pos);
	ret->set_data_ready(false);
	return ret;
}

void LRU_eviction_policy::access_page(thread_safe_page *pg,
		page_cell<thread_safe_page> &buf)
{
	/* move the page to the end of the pos vector. */
	int pos = buf.get_idx(pg);
	for (std::vector<int>::iterator it = pos_vec.begin();
			it != pos_vec.end(); it++) {
		if (*it == pos) {
			pos_vec.erase(it);
			break;
		}
	}
	pos_vec.push_back(pos);
}

thread_safe_page *LFU_eviction_policy::evict_page(
		page_cell<thread_safe_page> &buf)
{
	thread_safe_page *ret = NULL;
	int min_hits = 0x7fffffff;
	do {
		unsigned int num_io_pending = 0;
		for (unsigned int i = 0; i < buf.get_num_pages(); i++) {
			thread_safe_page *pg = buf.get_page(i);
			if (pg->get_ref()) {
				if (pg->is_io_pending())
					num_io_pending++;
				continue;
			}

			/*
			 * refcnt only increases within the lock of the cell,
			 * so if the page's refcnt is 0 above,
			 * it'll be always 0 within the lock.
			 */

			if (min_hits > pg->get_hits()) {
				min_hits = pg->get_hits();
				ret = pg;
			}

			/*
			 * if a page hasn't been accessed before,
			 * it's a completely new page, just use it.
			 */
			if (min_hits == 0)
				break;
		}
		if (num_io_pending == buf.get_num_pages()) {
			printf("all pages are at io pending\n");
			// TODO do something...
			// maybe we should use pthread_wait
		}
		/* it happens when all pages in the cell is used currently. */
	} while (ret == NULL);
	ret->set_data_ready(false);
	ret->reset_hits();
	return ret;
}

thread_safe_page *FIFO_eviction_policy::evict_page(
		page_cell<thread_safe_page> &buf)
{
	thread_safe_page *ret = buf.get_empty_page();
	/*
	 * This happens a lot if we actually read pages from the disk.
	 * So basically, we shouldn't use this eviction policy for SSDs
	 * or magnetic hard drive..
	 */
	while (ret->get_ref()) {
		ret = buf.get_empty_page();
	}
	ret->set_data_ready(false);
	return ret;
}

struct page_score
{
	thread_safe_page *pg;
	int score;
};

struct comp_flush_score {
	bool operator() (const page_score &pg1, const page_score &pg2) {
		return pg1.score < pg2.score;
	}
} flush_score_comparator;

void gclock_eviction_policy::assign_flush_scores(page_cell<thread_safe_page> &buf)
{
	const int num_pages = buf.get_num_pages();
	page_score pages[num_pages];
	int head = clock_head % num_pages;
	int num_avail_pages = 0;
	for (int i = 0; i < num_pages; i++) {
		thread_safe_page *pg = buf.get_page(i);
		if (pg->is_valid()) {
			int score = pg->get_hits() * num_pages + (i - head + num_pages) % num_pages;
			pg->set_flush_score(score);
			pages[num_avail_pages].pg = pg;
			pages[num_avail_pages].score = score;
			num_avail_pages++;
		}
	}
	// We need to normalize the flush score.
	std::sort(pages, pages + num_avail_pages, flush_score_comparator);
	for (int i = 0; i < num_avail_pages; i++) {
		pages[i].pg->set_flush_score(i);
	}
}

thread_safe_page *gclock_eviction_policy::evict_page(
		page_cell<thread_safe_page> &buf)
{
	thread_safe_page *ret = NULL;
	unsigned int num_referenced = 0;
	unsigned int num_dirty = 0;
	bool avoid_dirty = true;
	do {
		thread_safe_page *pg = buf.get_page(clock_head % buf.get_num_pages());
		if (num_dirty + num_referenced >= buf.get_num_pages()) {
			num_dirty = 0;
			num_referenced = 0;
			avoid_dirty = false;
		}
		clock_head++;
		if (pg->get_ref()) {
			num_referenced++;
			/*
			 * If all pages in the cell are referenced, we should
			 * return NULL to notify the invoker.
			 */
			if (num_referenced >= buf.get_num_pages())
				return NULL;
			continue;
		}
		if (avoid_dirty && pg->is_dirty()) {
			num_dirty++;
			continue;
		}
		if (pg->get_hits() == 0) {
			ret = pg;
			break;
		}
		pg->set_hits(pg->get_hits() - 1);
	} while (ret == NULL);
	ret->set_data_ready(false);
#if 0
	assign_flush_scores(buf);
#endif
	return ret;
}

/**
 * This method runs over all pages and finds the pages that are most likely
 * to be evicted. But we only return pages that have certain flags and/or
 * don't have certain pages.
 */
int gclock_eviction_policy::predict_evicted_pages(
		page_cell<thread_safe_page> &buf, int num_pages, int set_flags,
		int clear_flags, std::map<off_t, thread_safe_page *> &pages)
{
	// We are just predicting. We don't actually evict any pages.
	// So we need to make a copy of the hits of each page.
	short hits[CELL_SIZE];
	for (int i = 0; i < (int) buf.get_num_pages(); i++) {
		hits[i] = buf.get_page(i)->get_hits();
	}
	assign_flush_scores(buf);

	// The number of pages that are most likely to be evicted.
	int num_most_likely = 0;
	// The function returns when we get the expected number of pages
	// or we get pages
	while (true) {
		for (int i = 0; i < (int) buf.get_num_pages(); i++) {
			int idx = (i + clock_head) % buf.get_num_pages();
			short *hit = &hits[idx];
			// The page is already in the page map.
			if (*hit < 0)
				continue;
			else if (*hit == 0) {
				*hit = -1;
				thread_safe_page *p = buf.get_page(idx);
				if (p->test_flags(set_flags) && !p->test_flags(clear_flags)) {
					pages.insert(std::pair<off_t, thread_safe_page *>(
								p->get_offset(), p));
					if ((int) pages.size() == num_pages)
						return pages.size();
				}
				num_most_likely++;
			}
			else {
				(*hit)--;
			}
			/*
			 * We have got all pages that are most likely to be evicted.
			 * Let's just return whatever we have.
			 */
			if (num_most_likely >= MAX_NUM_WRITEBACK)
				return pages.size();
		}
	}
}

thread_safe_page *clock_eviction_policy::evict_page(
		page_cell<thread_safe_page> &buf)
{
	thread_safe_page *ret = NULL;
	unsigned int num_referenced = 0;
	unsigned int num_dirty = 0;
	bool avoid_dirty = true;
	do {
		thread_safe_page *pg = buf.get_page(clock_head % buf.get_num_pages());
		if (num_dirty + num_referenced >= buf.get_num_pages()) {
			num_dirty = 0;
			num_referenced = 0;
			avoid_dirty = false;
		}
		if (pg->get_ref()) {
			num_referenced++;
			if (num_referenced >= buf.get_num_pages())
				return NULL;
			clock_head++;
			continue;
		}
		if (avoid_dirty && pg->is_dirty()) {
			num_dirty++;
			clock_head++;
			continue;
		}
		if (pg->get_hits() == 0) {
			ret = pg;
			break;
		}
		pg->reset_hits();
		clock_head++;
	} while (ret == NULL);
	ret->set_data_ready(false);
	ret->reset_hits();
	return ret;
}

thread_safe_page *random_eviction_policy::evict_page(
		page_cell<thread_safe_page> &buf)
{
	thread_safe_page *ret = NULL;
	srand((unsigned int)time(0));
	int rand_head = 0;
	unsigned int num_referenced = 0;
	unsigned int num_dirty = 0;
	bool avoid_dirty = true;
	if (warm_up){
		thread_safe_page *pg = buf.get_page(rand_head % buf.get_num_pages());
		rand_head++;
		num_entry++;
		ret = pg;
		if (buf.get_num_pages() <= num_entry){
			warm_up = false;
		}
	}
	else do {
		rand_head = rand() % buf.get_num_pages();
		thread_safe_page *pg = buf.get_page(rand_head);

		if (num_dirty + num_referenced >= buf.get_num_pages()) {
			num_dirty = 0;
			num_referenced = 0;
			avoid_dirty = false;
		}
		if (pg->get_ref()) {
			num_referenced++;
			if (num_referenced >= buf.get_num_pages())
				return NULL;
			continue;
		}
		if (avoid_dirty && pg->is_dirty()) {
			num_dirty++;
			continue;
		}

		ret = pg;
	} while (ret == NULL);
	ret->set_data_ready(false);
	return ret;
}


// KH: APR : evict_page

void APR_eviction_policy::init()
{
	//leader = false;
	sample = false;
	clock_head = 0;
	lifo_head = 0;
	num_entry = 0;
	warm_up = true;
	default_policy = DEFAULT_POLICY;
	policy = default_policy;
	local_score = policy;
	time = 0;
	last_stime = 0;
	decay = DECAY_FACTOR_DEFAULT;
	curr_score = 0;

	int num_pages = params.get_SA_min_cell_size();
	lifo_page = new page_md_t[num_pages]();
	clock_page = new page_md_t[num_pages]();

	for (int i = 0; i < num_pages; i++){
		lifo_lt.push_back(i);
	}
#define MAX_POW_BIT	64
	/* pow_val[x] := base^(2^x) */
	pow_val = new double[MAX_POW_BIT]();

	pow_val[0] = decay;
	for (int i = 1; i < MAX_POW_BIT; i++)
		pow_val[i] = pow_val[i - 1] * pow_val[i - 1];
}

thread_safe_page *APR_eviction_policy::voter_clock_evict(
		page_cell<thread_safe_page> &buf)
{
	thread_safe_page *ret = NULL;
	unsigned int num_scan = 0;
	unsigned int num_referenced = 0;
	unsigned int num_dirty = 0;
	unsigned int num_scan_max = 2 * buf.get_num_pages();
	bool avoid_dirty = true;
	do {
		unsigned int clock_offset = clock_head % buf.get_num_pages();
#ifdef APR_DEBUG_PRINT
		std::cout << "CLOCK Offset : " << clock_offset << std::endl;
#endif
		thread_safe_page *pg = buf.get_page(clock_offset);
		num_scan++;
		clock_head++;

		if (num_dirty + num_referenced >= buf.get_num_pages()) {
			num_dirty = 0;
			num_referenced = 0;
			avoid_dirty = false;
		}

		if (pg->get_ref()) {
#ifdef APR_DEBUG_PRINT
			std::cout << "(CLOCK) Referenced : " << clock_offset << std::endl;
#endif
			num_referenced++;
			if (num_referenced >= buf.get_num_pages())
				return NULL;
			continue;
		}
		if (avoid_dirty && pg->is_dirty()) {
			num_dirty++;
			continue;
		}
		/* Look for clock victim */
		// TODO it must cause critical issue, at least it's unfair.
		if (test_and_clear_clock(pg)){
#ifdef APR_DEBUG_PRINT
			std::cout << "(CLOCK) Hit : " << clock_offset << std::endl;
#endif
			continue;
		}

		if (policy_clock()){
			if (clock_page[clock_offset].stale) {
#ifdef APR_DEBUG_PRINT
				std::cout << "(CLOCK) Staled (pevicted by lifo) : " << clock_offset << std::endl;
#endif
				end_challenge(global_ghost[pg->get_pg_offset()].stime, LIFO);
				global_ghost[pg->get_pg_offset()].present = false;
				clock_page[clock_offset].stale = false;
				continue;
			}
		}
		else {
			if (pg->evicted_by_clock()){
#ifdef APR_DEBUG_PRINT
				std::cout << "(CLOCK) Marked (already vevicted) : " << clock_offset << std::endl;
#endif
				continue;
			}
		}
		ret = pg;
		clock_head = clock_offset + 1;

	} while (ret == NULL && num_scan < num_scan_max);

	if (!default_policy && warm_up && ret->get_pg_offset() >= 0)
		warm_up = false;

	if (ret)
		ret->set_data_ready(false);
	return ret;
}


thread_safe_page *APR_eviction_policy::voter_lifo_evict(
		page_cell<thread_safe_page> &buf)
{
	thread_safe_page *ret = NULL;
	unsigned int num_scan = 0;
	unsigned int num_dirty = 0;
	unsigned int num_referenced = 0;
	unsigned int num_boundary = 0;
	unsigned int num_scan_max = 2 * buf.get_num_pages();
	bool avoid_dirty = true;
	bool avoid_boundary = true;

	// TODO change iterator to reverse_iterator (use auto)
//	std::list<int>::iterator lifo_iter = lifo_lt.end();
//	lifo_iter--;
	auto lifo_iter = lifo_lt.rbegin();
	lifo_iter++;

	if (warm_up) {
		thread_safe_page *pg = buf.get_page(lifo_head % buf.get_num_pages());
		lifo_head++;
		num_entry++;
		ret = pg;
		if (buf.get_num_pages() <= num_entry){
			warm_up = false;
		}
	}
	else do {
		unsigned int lifo_offset = *lifo_iter;
		lifo_iter++;

		if (lifo_offset == buf.get_num_pages())
			continue;

		thread_safe_page *pg = buf.get_page(lifo_offset);
		num_scan++;
#ifdef APR_DEBUG_PRINT
		std::cout << "LIFO Offset : " << lifo_offset << std::endl;
#endif
		if (num_dirty + num_referenced + num_boundary  >= buf.get_num_pages()){
			num_dirty = 0;
			num_boundary = 0;
			num_referenced = 0;
			avoid_dirty = false;
			avoid_boundary = false;
		}
		if (pg->get_ref()) {
#ifdef APR_DEBUG_PRINT
			std::cout << "(LIFO) Referenced : " << lifo_offset << std::endl;
#endif
			num_referenced++;
			if (num_referenced >= buf.get_num_pages())
				return NULL;
			continue;
		}
		if (avoid_dirty && pg->is_dirty()) {
			num_dirty++;
			continue;
		}
		if (pg->check_boundary()){
			if (avoid_boundary){
#ifdef APR_DEBUG_PRINT
				std::cout << "(LIFO) this is boundary page : " <<
					pg->get_pg_offset() << " (" << lifo_offset << ")" << std::endl;
#endif
				num_boundary++;
				continue;
			}
			else pg->clear_boundary();
		}

		if (policy_lifo()) {
			if(lifo_page[lifo_offset].stale) {
#ifdef APR_DEBUG_PRINT
				std::cout << "(LIFO) Stale (pevicted by clock) : " << lifo_offset << std::endl;
#endif
				end_challenge(global_ghost[pg->get_pg_offset()].stime, CLOCK);
				global_ghost[pg->get_pg_offset()].present = false;
				lifo_page[lifo_offset].stale = false;
				continue;
			}
		}
		else {
			if (pg->evicted_by_lifo()){
#ifdef APR_DEBUG_PRINT
				std::cout << "(LIFO) Marked (already vevicted) : " << lifo_offset << std::endl;
#endif
				if (num_referenced >= buf.get_num_pages())
					return NULL;
				continue;
			}
		}
		ret = pg;
		lifo_head = lifo_offset;

	} while (ret == NULL && num_scan < num_scan_max);
	if (ret)
		ret->set_data_ready(false);
	return ret;
}

thread_safe_page *APR_eviction_policy::voter_actual_victim(
		thread_safe_page *clock, thread_safe_page *lifo)
{
	thread_safe_page *ret = NULL;
	if (clock == lifo){
		if (clock) {
#ifdef APR_DEBUG_PRINT
			std::cout << "Same Victim" << std::endl;
#endif
			if (clock->is_evicted()){
				if (clock->evicted_by_lifo()){		/* v-evicted by lifo */
					end_challenge(clock->get_stime(), LIFO);
				}
				else {
					end_challenge(clock->get_stime(), CLOCK);
				}
			}
			remove_page(clock, CLOCK);
			update_lifo_list(clock_head-1);
			ret = clock;
		}
	}
	else {
		/* Actual policy is clock */
		if (policy_clock()) {
			if (clock) {
				if (clock->is_evicted()){
					if (clock->evicted_by_clock()){
						pevict_page(clock);
					}
					else {
						end_challenge(clock->get_stime(), LIFO);
						remove_page(clock, CLOCK);
						update_lifo_list(clock_head-1);
					}
				}
				else {
					start_challenge(clock, CLOCK);
					pevict_page(clock);
				}
				ret = clock;
			}
			if (lifo) {
				if (lifo->is_evicted() || lifo_page[lifo_head].stale) {
					/* Already p-evicted by clock */
					if (lifo_page[lifo_head].stale){
#ifdef APR_DEBUG_PRINT
						std::cout << "LIFO victim ("<< clock_head-1 <<
							") was already p-evicted by clock" << std::endl;
#endif
						remove_page(lifo, LIFO);
						update_lifo_list(lifo_head);
					}
					else {
#ifdef APR_DEBUG_PRINT
						std::cout << "LIFO victim ("<< clock_head-1 <<
							") was already v-evicted by clock" << std::endl;
#endif
						refresh_chal(lifo, lifo_head);
					}
					end_challenge(lifo->get_stime(), CLOCK);
				}
				else {
					start_challenge(lifo, LIFO);
				}
			}
		}
		/* Actual policy is lifo */
		else {
			if (lifo) {
				if (lifo->is_evicted()) {
					if (lifo->evicted_by_lifo()) {
						pevict_page(lifo);
					}
					else {
						end_challenge(lifo->get_stime(), CLOCK);
						remove_page(lifo, LIFO);
						update_lifo_list(lifo_head);
					}
				}
				else {
					start_challenge(lifo, LIFO);
					pevict_page(lifo);
				}
				ret = lifo;
			}
			if (clock) {
				if (clock->is_evicted() || clock_page[clock_head-1].stale){
					/* Already p-evicted by lifo */
					if (clock_page[clock_head-1].stale){
#ifdef APR_DEBUG_PRINT
						std::cout << "Clock victim ("<< clock_head-1 <<
							") was already p-evicted by LIFO" << std::endl;
#endif
						remove_page(clock, CLOCK);
						update_lifo_list(lifo_head);
					}
					else {
#ifdef APR_DEBUG_PRINT
						std::cout << "Clock victim ("<< clock_head-1 <<
							") was already v-evicted by LIFO" << std::endl;
#endif
						refresh_chal(clock, clock_head-1);
					}
					end_challenge(clock->get_stime(), LIFO);
				}
				else {
					start_challenge(clock, CLOCK);
				}
			}
		}
	}

	return ret;
}

thread_safe_page *APR_eviction_policy::follower_clock_evict(
		page_cell<thread_safe_page> &buf)
{
	thread_safe_page *ret = NULL;
	unsigned int num_referenced = 0;
	unsigned int num_dirty = 0;
	bool avoid_dirty = true;
	do {
		thread_safe_page *pg = buf.get_page(clock_head % buf.get_num_pages());
		if (num_dirty + num_referenced >= buf.get_num_pages()) {
			num_dirty = 0;
			num_referenced = 0;
			avoid_dirty = false;
		}
		if (pg->get_ref()) {
			num_referenced++;
			if (num_referenced >= buf.get_num_pages())
				return NULL;
			clock_head++;
			continue;
		}
		if (avoid_dirty && pg->is_dirty()) {
			num_dirty++;
			clock_head++;
			continue;
		}
		if (pg->get_hits() == 0) {
			ret = pg;
			//lifo_lt.remove(clock_head);
			//lifo_lt.push_back(clock_head);
			break;
		}
		pg->reset_hits();
		clock_head++;
	} while (ret == NULL);
	ret->set_data_ready(false);
	ret->reset_hits();
	return ret;
}

thread_safe_page *APR_eviction_policy::follower_lifo_evict(
		page_cell<thread_safe_page> &buf)
{
	thread_safe_page *ret = NULL;
	unsigned int num_boundary = 0;
	unsigned int num_dirty = 0;
	unsigned int num_referenced = 0;
	bool avoid_dirty = true;
	bool avoid_boundary = true;

	auto lifo_iter = lifo_lt.rbegin();
	lifo_iter++;

	if (warm_up) {
		assert(default_policy == LIFO);
		thread_safe_page *pg = buf.get_page(lifo_head % buf.get_num_pages());
		lifo_head++;
		num_entry++;
		ret = pg;
		if (buf.get_num_pages() <= num_entry){
			warm_up = false;
		}
	}
	else do{
		unsigned int lifo_offset = *lifo_iter;
		lifo_iter++;

		if (lifo_offset == buf.get_num_pages())
			continue;

		thread_safe_page *pg = buf.get_page(lifo_offset);

		if (num_referenced + num_dirty + num_boundary >= buf.get_num_pages()) {
			num_referenced = 0;
			num_dirty = 0;
			num_boundary = 0;
			avoid_dirty = false;
			avoid_boundary = false;
		}
		if (pg->get_ref()) {
			num_referenced++;
			if (num_referenced >= buf.get_num_pages())
				return NULL;
			continue;
		}
		if (avoid_dirty && pg->is_dirty()) {
			num_dirty++;
			continue;
		}
		if (pg->check_boundary()){
			if (avoid_boundary){
				num_boundary++;
				continue;
			}
			else pg->clear_boundary();
		}

		ret = pg;
		lifo_head = lifo_offset;
		lifo_lt.remove(lifo_head);
		lifo_lt.push_back(lifo_head);
	} while (ret == NULL);
	ret->set_data_ready(false);
	return ret;

}

void APR_eviction_policy::update_score()
{
	if (curr_score){
		double old = global_score.load();
		double desired = old + curr_score;
		while(!global_score.compare_exchange_weak(old, desired,
					std::memory_order_release, std::memory_order_relaxed)){
			desired = old + curr_score;
		}

		if (global_policy == CLOCK && desired > 0)
			global_policy = LIFO;
		else if (global_policy == LIFO && desired <=0)
			global_policy = CLOCK;
		//std::cout << global_policy << std::endl;

#ifdef APR_DEBUG_PRINT
		std::cout << "Local Score : " << local_score << std::endl;
		std::cout << "Global Score : " << old;
		std::cout << " -> " << old + curr_score << std::endl;
#endif
		print_lock.lock();
		std::cout << old + curr_score << std::endl;
		print_lock.unlock();
	}
}

//#ifdef APR_SAMPLING
thread_safe_page *APR_eviction_policy::evict_page(
		page_cell<thread_safe_page> &buf, off_t offset)
{
	int npages = buf.get_num_pages();
	thread_safe_page *ret = NULL;
	curr_score = 0;

	if (this->sample) {
#ifdef APR_DEBUG_PRINT
		pthread_t th_id;
		th_id = pthread_self();
		std::cout << "\nHashcell: " << hash << " (Thread " << th_id << ")" << std::endl;
		std::cout << "CACHE MISS: " << offset << std::endl;
		std::cout << "Global policy is " << (global_policy ? "LIFO" : "CLOCK") << std::endl;
#endif
		thread_safe_page *lifo_victim, *clock_victim, *actual_victim;

		if(local_score > 0)
			policy = LIFO;
		else policy = CLOCK;

#ifdef APR_DEBUG_PRINT
		std::cout << "Local policy is " << (policy ? "LIFO" : "CLOCK") << std::endl;
#endif

		/* Check whether the page is in challenging state as a ghost page */
		if(is_ghost(offset)){
			/* If so, calculate the reward and reflect it into local score */
			eval_challenge(offset);
		}

		/* if warm-up process is proceeding */
		if (warm_up) {
			/* fill up entries under clock policy */
			if (default_policy == CLOCK)
				actual_victim = voter_clock_evict(buf);
			else if (default_policy == LIFO)
				actual_victim = voter_lifo_evict(buf);
		}
		else {
			clock_victim = voter_clock_evict(buf);
#ifdef APR_DEBUG_PRINT
			if (clock_victim)
				std::cout << "CLOCK Victim: " << clock_victim->get_pg_offset()
					<< " ("<< (clock_head - 1) % npages << ")" << std::endl;
			else std::cout << "CLOCK Victim is NULL" << std::endl;
#endif

			lifo_victim = voter_lifo_evict(buf);

#ifdef APR_DEBUG_PRINT
			if (lifo_victim)
				std::cout << "LIFO Victim: " << lifo_victim->get_pg_offset()
					<< " ("<< lifo_head << ")" << std::endl;
			else std::cout << "LIFO Victim is NULL" << std::endl;
#endif

			actual_victim = voter_actual_victim(clock_victim, lifo_victim);

			time++;
#ifdef APR_DEBUG_PRINT
			std::cout << "Local Score decay: " << local_score;
#endif
			local_score = local_score * decay;
#ifdef APR_DEBUG_PRINT
			std::cout << " -> " << local_score << std::endl;
#endif
		}

		local_score += curr_score;
#ifdef APR_SAMPLING
		update_score();
		time_check++;
		if (!(time_check % APR_SAMPLE_SIZE)) {
			/* Decay the global score */
#ifdef APR_DEBUG_PRINT
			std::cout << "Score decay: " << global_score.load();
#endif
			global_score = global_score * decay;
#ifdef APR_DEBUG_PRINT
			std::cout << " -> " << global_score.load() << std::endl;
#endif
		}
#endif
		ret = actual_victim;
	}
	else {
		if (!warm_up)
			policy = global_policy;
		if(policy_clock())
			ret = follower_clock_evict(buf);
		else
			ret = follower_lifo_evict(buf);
	}

	return ret;
}
/* no sampling
#else
thread_safe_page *APR_eviction_policy::evict_page(
		page_cell<thread_safe_page> &buf, off_t offset)
{
	int npages = buf.get_num_pages();
	struct timespec local_time[2];

	thread_safe_page *ret = NULL;
	thread_safe_page *lifo_victim, *clock_victim;


	update_policy();

#ifdef APR_DEBUG_TIME_DETAIL
	clock_gettime(CLOCK_MONOTONIC, &local_time[0]);
#endif

	clock_victim = clock_evict_page(buf);		// get clock victim

#ifdef APR_DEBUG_TIME_DETAIL
	clock_gettime(CLOCK_MONOTONIC, &local_time[1]);
	calclock(local_time, &cache_time.pol_clock_t[0], &cache_time.pol_clock_c[0]);
#endif

#ifdef APR_DEBUG_PRINT
	if (clock_victim)
		std::cout << "CLOCK Victim: " << clock_victim->get_pg_offset()
			<< " ("<< (clock_head - 1) % npages << ")" << std::endl;
		else std::cout << "CLOCK Victim is NULL" << std::endl;
#endif

	if (!warm_up){
#ifdef APR_DEBUG_TIME_DETAIL
		clock_gettime(CLOCK_MONOTONIC, &local_time[0]);
#endif
		lifo_victim = lifo_evict_page(buf);			// get lifo victim

#ifdef APR_DEBUG_TIME_DETAIL
		clock_gettime(CLOCK_MONOTONIC, &local_time[1]);
		calclock(local_time, &cache_time.pol_lifo_t[0], &cache_time.pol_lifo_c[0]);
#endif

#ifdef APR_DEBUG_PRINT
		if (lifo_victim)
			std::cout << "LIFO Victim: " << lifo_victim->get_pg_offset()
				<< " ("<< lifo_head << ")" << std::endl;
		else std::cout << "LIFO Victim is NULL" << std::endl;
#endif

#ifdef APR_DEBUG_TIME_DETAIL
		clock_gettime(CLOCK_MONOTONIC, &local_time[0]);
#endif
		ret = actual_victim(clock_victim, lifo_victim);	// get actual victim
#ifdef APR_DEBUG_TIME_DETAIL
		clock_gettime(CLOCK_MONOTONIC, &local_time[1]);
		calclock(local_time, &cache_time.pol_compete_t[0], &cache_time.pol_compete_c[0]);
#endif
	}

	return (warm_up ? clock_victim : ret);
}
#endif
*/
thread_safe_page *APR_eviction_policy::evict_page(
		page_cell<thread_safe_page> &buf)
{
	std::cout << "wrong path for APR" << std::endl;
	exit(1);
}


void APR_eviction_policy::update_policy(void)
{
#ifdef APR_DEBUG_PRINT
	std::cout << "SCORE: " << curr_score;
#endif
	if (curr_score > 0){
		policy = LIFO;

#ifdef APR_DEBUG_PRINT
		std::cout << "  -> POLICY: LIFO" << std::endl;
#endif
	}
	else {
		policy = CLOCK;
#ifdef APR_DEBUG_PRINT
		std::cout << "  -> POLICY: CLOCK" << std::endl;
#endif
	}

}

thread_safe_page *APR_eviction_policy::clock_evict_page(
		page_cell<thread_safe_page> &buf)
{
	thread_safe_page *ret = NULL;
//	unsigned int num_io_pending = 0;
	unsigned int num_referenced = 0;
	unsigned int num_dirty = 0;
	bool avoid_dirty = true;
	do {
		unsigned int clock_offset = clock_head % buf.get_num_pages();
#ifdef APR_DEBUG_PRINT
		std::cout << "CLOCK Offset : " << clock_offset << std::endl;
#endif
		thread_safe_page *pg = buf.get_page(clock_offset);
		clock_head++;

		if (num_dirty + num_referenced >= buf.get_num_pages()) {
			num_dirty = 0;
			num_referenced = 0;
			avoid_dirty = false;
		}
		if (pg->get_ref()) {
#ifdef APR_DEBUG_PRINT
			std::cout << "(CLOCK) Referenced : " << clock_offset << std::endl;
#endif
			num_referenced++;
			if (num_referenced >= buf.get_num_pages())
				return NULL;
			continue;
		}
		if (avoid_dirty && pg->is_dirty()) {
			num_dirty++;
			continue;
		}
		/* Look for clock victim */
		if (test_and_clear_clock(pg)){
#ifdef APR_DEBUG_PRINT
			std::cout << "(CLOCK) Hit : " << clock_offset << std::endl;
#endif
			continue;
		}

		if (policy_clock()){
			if (clock_page[clock_offset].stale) {
#ifdef APR_DEBUG_PRINT
				std::cout << "(CLOCK) Staled (pevicted by lifo) : " << clock_offset << std::endl;
#endif
				end_challenge(global_ghost[pg->get_pg_offset()].stime, LIFO);
//				end_challenge(LIFO);
				global_ghost[pg->get_pg_offset()].present = false;
				clock_page[clock_offset].stale = false;
				continue;
			}
		}
		else {
			if (pg->evicted_by_clock()){
#ifdef APR_DEBUG_PRINT
				std::cout << "(CLOCK) Marked (already vevicted) : " << clock_offset << std::endl;
#endif
				continue;
			}
		}
		ret = pg;
		clock_head = clock_offset + 1;

	} while (ret == NULL);

	if (warm_up && ret->get_pg_offset() >= 0)
		warm_up = false;

	ret->set_data_ready(false);
	return ret;
}


thread_safe_page *APR_eviction_policy::lifo_evict_page(
		page_cell<thread_safe_page> &buf)
{
	thread_safe_page *ret = NULL;
	unsigned int num_dirty = 0;
	unsigned int num_referenced = 0;
	bool avoid_dirty = true;

	std::list<int>::iterator lifo_iter = lifo_lt.end();
	lifo_iter--;

	do {
		unsigned int lifo_offset = *lifo_iter;
		lifo_iter--;

		if (lifo_offset == buf.get_num_pages())
			continue;

		thread_safe_page *pg = buf.get_page(lifo_offset);
#ifdef APR_DEBUG_PRINT
		std::cout << "LIFO Offset : " << lifo_offset << std::endl;
#endif
		if (num_dirty + num_referenced  >= buf.get_num_pages()){
			num_dirty = 0;
			num_referenced = 0;
			avoid_dirty = false;
		}
		if (pg->get_ref()) {
#ifdef APR_DEBUG_PRINT
			std::cout << "(LIFO) Referenced : " << lifo_offset << std::endl;
#endif
			num_referenced++;
			if (num_referenced >= buf.get_num_pages())
				return NULL;
			continue;
		}
		if (avoid_dirty && pg->is_dirty()) {
			num_dirty++;
			continue;
		}
/*		TODO it might be needed for some algorithms such as cycle_tirangle
		if (test_and_clear_lifo(pg)){
			num_referenced++;
#ifdef APR_DEBUG_PRINT
			std::cout << "(LIFO) Hit : " << lifo_offset << std::endl;
#endif
			continue;
		}
*/
		if (policy_lifo()) {
			if(lifo_page[lifo_offset].stale) {
#ifdef APR_DEBUG_PRINT
				std::cout << "(LIFO) Stale (pevicted by clock) : " << lifo_offset << std::endl;
#endif
				end_challenge(global_ghost[pg->get_pg_offset()].stime, CLOCK);
//				end_challenge(CLOCK);
				global_ghost[pg->get_pg_offset()].present = false;
				lifo_page[lifo_offset].stale = false;
				continue;
			}
		}
		else {
			if (pg->evicted_by_lifo()){
#ifdef APR_DEBUG_PRINT
				std::cout << "(LIFO) Marked (already vevicted) : " << lifo_offset << std::endl;
#endif
				continue;
			}
		}
		ret = pg;
		lifo_head = lifo_offset;

	} while (ret == NULL);
	ret->set_data_ready(false);
	return ret;
}

thread_safe_page *APR_eviction_policy::actual_victim(
		thread_safe_page *clock, thread_safe_page *lifo)
{
	thread_safe_page *ret = NULL;
	if (clock == lifo){
		if (clock) {
#ifdef APR_DEBUG_PRINT
			std::cout << "Same Victim" << std::endl;
#endif
			if (clock->is_evicted()){
				if (clock->evicted_by_lifo()){		/* v-evicted by lifo */
					end_challenge(clock->get_stime(), LIFO);
//					end_challenge(LIFO);
				}
				else {
					end_challenge(clock->get_stime(), CLOCK);
//					end_challenge(CLOCK);
				}
			}
			remove_page(clock, CLOCK);
			ret = clock;
		}
	}
	else {
		/* Actual policy is clock */
		if (policy_clock()) {
			if (clock) {
				if (clock->is_evicted()){
					if (clock->evicted_by_clock()){
						pevict_page(clock);
					}
					else {
						end_challenge(clock->get_stime(), LIFO);
						remove_page(clock, CLOCK);
					}
				}
				else {
					start_challenge(clock, CLOCK);
					pevict_page(clock);
				}
				ret = clock;
			}
			if (lifo) {
				if (lifo->is_evicted() || lifo_page[lifo_head].stale) {
					end_challenge(lifo->get_stime(), CLOCK);
					/* Already p-evicted by clock */
					if (lifo_page[lifo_head].stale){
						remove_page(lifo, LIFO);
					}
					else {
						refresh_chal(lifo, lifo_head);
					}
				}
				else {
					start_challenge(lifo, LIFO);
				}
			}
		}
		/* Actual policy is lifo */
		else {
			if (lifo) {
				if (lifo->is_evicted()){
					if (lifo->evicted_by_lifo())
						pevict_page(lifo);
					else {
						end_challenge(lifo->get_stime(), CLOCK);
						remove_page(lifo, LIFO);
					}
				}
				else {
					start_challenge(lifo, LIFO);
					pevict_page(lifo);
				}
				ret = lifo;
			}
			if (clock) {
				if (clock->is_evicted() || clock_page[clock_head-1].stale){
					end_challenge(clock->get_stime(), LIFO);
					/* Already p-evicted by lifo */
					if (clock_page[clock_head-1].stale){
						remove_page(clock, CLOCK);
					}
					else {
						refresh_chal(clock, clock_head-1);
					}
				}
				else {
					start_challenge(clock, CLOCK);
				}
			}
		}
	}

	return ret;
}

void APR_eviction_policy::update_lifo_list(int pg_idx)
{
#ifdef APR_DEBUG_PRINT
	std::cout << pg_idx << " is replaced in LIFO list" << std::endl;
#endif
/*
	lifo_lt.remove(pg_idx);
	lifo_lt.push_back(pg_idx);
*/
	for (auto it = lifo_lt.rbegin(); it != lifo_lt.rend(); it++)
	{
		if(*it == pg_idx) {
			lifo_lt.erase(--(it.base()));
			lifo_lt.push_back(pg_idx);
			break;
		}
	}
}

void APR_eviction_policy::remove_page(
		thread_safe_page *pg, bool pol)
{
	unsigned int pg_idx = (pol ? lifo_head : clock_head - 1);
	//update_lifo_list(pg_idx);

	refresh_chal(pg, pg_idx);
}

void APR_eviction_policy::refresh_chal(
		thread_safe_page *pg, int pg_idx)
{
	clock_page[pg_idx].stale = false;
	lifo_page[pg_idx].stale = false;
	pg->clear_evicted();

	global_ghost[pg->get_pg_offset()].present = false;
}


void APR_eviction_policy::start_challenge(
		thread_safe_page *pg, bool policy)
{
	assert(!pg->is_evicted());

#ifdef APR_DEBUG_PRINT
	std::cout << "Challenge Start (by " << (policy?"LIFO : ":"CLOCK :")
		<< (policy? lifo_head : clock_head-1) << ")" << std::endl;
#endif
	if (policy == LIFO) { // TODO Check this
		lifo_lt.remove(lifo_head);
		lifo_lt.push_front(lifo_head);
	}

	pg->set_evicted(policy);
	pg->set_stime(time);
}
/*
bool APR_eviction_policy::hit_check(
		thread_safe_page *pg)
{
	curr_score = 0;
	if (pg->is_evicted()){
		if (pg->evicted_by_lifo()){
#ifdef APR_DEBUG_PRINT
			std::cout << "This was v-evicted by LIFO" << std::endl;
#endif
			end_challenge(pg->get_stime(), CLOCK);
		}
		else {
#ifdef APR_DEBUG_PRINT
			std::cout << "This was v-evicted by CLOCK" << std::endl;
#endif
			end_challenge(pg->get_stime(), LIFO);
		}
		update_score();
	}
}
*/

bool APR_eviction_policy::test_and_clear_clock(
		thread_safe_page *pg)
{
	bool ref, cref;

	ref = pg->test_and_clear_page_ref();

	if (!is_ghost(pg->get_pg_offset())) {
		if (ref = pg->test_and_clear_page_ref() ) {
			if (pg->is_evicted()){
				eval_challenge(pg);
			}
			pg->clear_evicted();
		}
	}
#ifdef APR_DEBUG_PRINT
	else {
		std::cout << "This page was pevicted by " <<
			((global_ghost[pg->get_pg_offset()].policy==LIFO)?"LIFO":"CLOCK") << std::endl;
	}
#endif


	return ref;
}

bool APR_eviction_policy::test_and_clear_lifo(
		thread_safe_page *pg)
{
	bool ref, lref;
	ref = pg->test_and_clear_page_ref();
	lref = pg->test_and_clear_page_lref();

	if (ref) {
		pg->set_cref();
		if (pg->is_evicted()){		/* v-evicted, but referenced in the meantime */
			eval_challenge(pg);
		}
		pg->clear_evicted();
	}

	return ref | lref;
}

void APR_eviction_policy::pevict_page(
		thread_safe_page *pg)
{
	int pg_idx = (policy_lifo()?lifo_head:clock_head-1);
#ifdef APR_DEBUG_PRINT
	std::cout << pg_idx << " is p-evicted by "
		<< (policy_lifo()?"LIFO":"CLOCK") << std::endl;
#endif
	pg->clear_evicted();

	/* CLOCK evict a page
	 * make stale the lifo page	*/
	if (policy_clock()) {
		lifo_page[pg_idx].stale = true;
	}
	/* LIFO evict a page
	 * make stale the clock page */
	else {
		clock_page[pg_idx].stale = true;
		/* replace lifo list when lifo p-evicted the page */
		update_lifo_list(pg_idx);
	}

	/* Unless cold miss, register the page as a ghost */
	if (pg->get_pg_offset() >= 0)
		reg_ghost(pg, pg_idx);
}

void APR_eviction_policy::reg_ghost(
		thread_safe_page *pg, int pg_idx)
{
	off_t offset = pg->get_pg_offset();

	/* Store informations of physically evicted page */
	global_ghost[offset].present = true;
	global_ghost[offset].policy = policy;
	global_ghost[offset].stime = time;
	global_ghost[offset].pg_idx = pg_idx;

#ifdef APR_DEBUG_PRINT
	std::cout << "Register Ghost : " << offset << " (time: " << time << ")" << std::endl;
#endif
}

/* To handle the physically evicted page */
bool APR_eviction_policy::eval_challenge(off_t offset)
{
	bool winner;
	/* Discover Which policy win for the ghost page */
	if(global_ghost[offset].policy == LIFO)
		winner = CLOCK;
	else winner = LIFO;

#ifdef APR_DEBUG_PRINT
	std::cout << "GHOST was evicted by " << (!winner ? "LIFO" : "CLOCK");
	std::cout << " (time: " << global_ghost[offset].stime << ")" << std::endl;
#endif

	/* Update the local score */
	end_challenge(global_ghost[offset].stime, winner);

	/* Refresh the challenge states */
	global_ghost[offset].present = false;

	if (winner == CLOCK)
		clock_page[global_ghost[offset].pg_idx].stale = false;
	else
		lifo_page[global_ghost[offset].pg_idx].stale = false;
}

/* To handle the physically evicted page */
void APR_eviction_policy::end_challenge(unsigned long stime, bool winner)
{
	double reward;
#ifdef APR_DEBUG_PRINT
	std::cout << "Start time of the challenge: " << stime;
	std::cout << " / Current time: " << time << std::endl;
#endif
	reward = spow(decay, time - stime);

	if (winner == LIFO)
		curr_score += reward;
	else
		curr_score -= reward;

#ifdef APR_DEBUG_PRINT
	std::cout << ((winner==LIFO)?"LIFO win!":"CLOCK win!");
	std::cout << " / Reward: " <<((winner==LIFO)?"+":"-") << reward;
	std::cout << " -> Curr Score: " << curr_score << std::endl;
#endif
}

/* To handle the virtually evicted page */
bool APR_eviction_policy::eval_challenge(thread_safe_page *pg)
{
	bool winner;
	/* Discover which policy wins on the ghost page */
	if (pg->evicted_by_lifo())
		winner = CLOCK;
	else if (pg->evicted_by_clock())
		winner = LIFO;
	else
		assert(false);

	/* Update the local score */
	end_challenge(pg->get_stime(), winner);

	/* Refresh the challenge states */
	//pg->clear_evicted();
	//refresh_chal(pg, pg_idx);
}

/* To handle the virtually evicted page */
void APR_eviction_policy::end_challenge(bool winner)
{
	double reward = 1;

	if (winner == LIFO)
		curr_score += reward;
	else
		curr_score -= reward;

#ifdef APR_DEBUG_PRINT
	std::cout << "Score:  " << curr_score << "\nReward: " << reward << std::endl;
#endif
}

double APR_eviction_policy::spow(double base, unsigned long power)
{
	double res = 1;
	int bit;

	for (bit = 0; bit <= MAX_POW_BIT; bit++){
		if (power & 0x1UL)
			res = res * pow_val[bit];
		power = power >> 1;
	}

	return res;
}

bool APR_eviction_policy::is_ghost(off_t offset)
{
	return global_ghost[offset].present;
}


associative_cache::~associative_cache()
{

#ifdef APR_DEBUG_STAT
	for (int i = 0; i < get_num_cells(); i++) {
			hash_cell *cell = get_cell(i);
			APR_global_stats.cache_access += cell->APR_cache_access();
			APR_global_stats.hit_count += cell->APR_hit_count();
			APR_global_stats.miss_count += cell->APR_miss_count();
			APR_global_stats.cold_miss += cell->APR_cold_miss();
	}
/*
	for (int i = 0; i < get_file_size(APR_FILE_NAME)/PAGE_SIZE; i++){
		std::cout << access_log[i] << std::endl;
	}
*/

	std::cout << "[-CACHE STATISTICS-]" << std::endl;
	std::cout << "cache access: " << APR_global_stats.cache_access << std::endl;
	std::cout << "cache hit: " << APR_global_stats.hit_count << std::endl;
	std::cout << "cache miss: " << APR_global_stats.miss_count << std::endl;
	std::cout << "cold miss: " << APR_global_stats.cold_miss << std::endl;
	std::cout << "hit rate (w/o cold miss): " <<
		(float)APR_global_stats.hit_count/(float)APR_global_stats.cache_access << std::endl;
#endif

#ifdef APR_DEBUG_TIME
	std::cout << "total(ms): " << cache_time.total_t/MILLION << std::endl;
#endif
#ifdef APR_DEBUG_TIME_DETAIL
	std::cout << "clock(ms): " << cache_time.pol_clock_t[0]/MILLION << " (" << cache_time.pol_clock_c[0] << ")" << std::endl;
	std::cout << "lifo(ms): " << cache_time.pol_lifo_t[0]/MILLION << " (" << cache_time.pol_lifo_c[0] << ")" << std::endl;
	std::cout << "competetion(ms): " << cache_time.pol_compete_t[0]/MILLION << " (" << cache_time.pol_compete_c[0] << ")" << std::endl;
#endif
	for (unsigned int i = 0; i < cells_table.size(); i++){
		if (cells_table[i])
			hash_cell::destroy_array(cells_table[i], init_ncells);
	}
	manager->unregister_cache(this);
	memory_manager::destroy(manager);


#ifdef USE_APR
	delete[] global_ghost;
#endif
}


bool associative_cache::shrink(int npages, char *pages[])
{
	if (flags.set_flag(TABLE_EXPANDING)) {
		/*
		 * if the flag has been set before,
		 * it means another thread is expanding the table,
		 */
		return false;
	}

	/* starting from this point, only one thred can be here. */

	int pg_idx = 0;
	int orig_ncells = get_num_cells();
	while (pg_idx < npages) {
		// The cell table isn't in the stage of splitting.
		if (split == 0) {
			hash_cell *cell = get_cell(expand_cell_idx);
			while (height >= params.get_SA_min_cell_size()) {
				int num = max(0, cell->get_num_pages() - height);
				num = min(npages - pg_idx, num);
				if (num > 0) {
					cell->steal_pages(&pages[pg_idx], num);
					pg_idx += num;
				}

				if (expand_cell_idx <= 0) {
					height--;
					expand_cell_idx = orig_ncells;
				}
				expand_cell_idx--;
				cell = get_cell(expand_cell_idx);
			}
			if (pg_idx == npages) {
				cache_npages.dec(npages);
				flags.clear_flag(TABLE_EXPANDING);
				return true;
			}
		}

		/* From here, we shrink the cell table. */

		// When the thread is within in the while loop, other threads can
		// hardly access the cells in the table.
		if (level == 0)
			break;
		int num_half = (1 << level) * init_ncells / 2;
		table_lock.write_lock();
		if (split == 0) {
			split = num_half - 1;
			level--;
		}
		table_lock.write_unlock();
		while (split > 0) {
			hash_cell *high_cell = get_cell(split + num_half);
			hash_cell *cell = get_cell(split);
			// At this point, the high cell and the low cell together
			// should have no more than CELL_MIN_NUM_PAGES pages.
			cell->merge(high_cell);
			table_lock.write_lock();
			split--;
			table_lock.write_unlock();
		}
		int orig_narrays = (1 << level);
		// It's impossible to access the arrays after `narrays' now.
		int narrays = orig_narrays / 2;
		for (int i = narrays; i < orig_narrays; i++) {
			hash_cell::destroy_array(cells_table[i], init_ncells);
			cells_table[i] = NULL;
		}
	}
	flags.clear_flag(TABLE_EXPANDING);
	cache_npages.dec(npages);
	return true;
}

/**
 * This method increases the cache size by `npages'.
 */
int associative_cache::expand(int npages)
{
	if (flags.set_flag(TABLE_EXPANDING)) {
		/*
		 * if the flag has been set before,
		 * it means another thread is expanding the table,
		 */
		return 0;
	}

	/* starting from this point, only one thred can be here. */

	char *pages[npages];
	if (!manager->get_free_pages(npages, pages, this)) {
		flags.clear_flag(TABLE_EXPANDING);
		fprintf(stderr, "expand: can't allocate %d pages\n", npages);
		return 0;
	}
	int pg_idx = 0;
	bool expand_over = false;
	while (pg_idx < npages && !expand_over) {
		// The cell table isn't in the stage of splitting.
		if (split == 0) {
			int orig_ncells = get_num_cells();
			/* We first try to add pages to the existing cells. */
			hash_cell *cell = get_cell(expand_cell_idx);
			while (height <= CELL_SIZE && pg_idx < npages) {
				int num_missing;

				assert(pages[pg_idx]);
				// We should skip the cells with more than `height'.
				if (cell->get_num_pages() >= height)
					goto next_cell;

				num_missing = height - cell->get_num_pages();
				num_missing = min(num_missing, npages - pg_idx);
				cell->add_pages(&pages[pg_idx], num_missing);
				pg_idx += num_missing;
next_cell:
				expand_cell_idx++;
				if (expand_cell_idx >= (unsigned) orig_ncells) {
					expand_cell_idx = 0;
					height++;
				}
				cell = get_cell(expand_cell_idx);
			}
			if (pg_idx == npages) {
				cache_npages.inc(npages);
				flags.clear_flag(TABLE_EXPANDING);
				return npages;
			}

			/* We have to expand the cell table in order to add more pages. */

			/* Double the size of the cell table. */
			/* create cells and put them in a temporary table. */
			std::vector<hash_cell *> table;
			int orig_narrays = (1 << level);
			for (int i = orig_narrays; i < orig_narrays * 2; i++) {
				hash_cell *cells = hash_cell::create_array(node_id, init_ncells);
				printf("create %d cells: %p\n", init_ncells, cells);
				for (int j = 0; j < init_ncells; j++) {
					cells[j].init(this, i * init_ncells + j, false);
				}
				table.push_back(cells);
			}
			/*
			 * here we need to hold the lock because other threads
			 * might be accessing the table. by using the write lock,
			 * we notify others the table has been changed.
			 */
			table_lock.write_lock();
			for (unsigned int i = 0; i < table.size(); i++) {
				cells_table[orig_narrays + i] = table[i];
			}
			table_lock.write_unlock();
		}
		height = params.get_SA_min_cell_size() + 1;

		// When the thread is within in the while loop, other threads
		// can hardly access the cells in the table.
		int num_half = (1 << level) * init_ncells;
		while (split < num_half) {
			hash_cell *expanded_cell = get_cell(split + num_half);
			hash_cell *cell = get_cell(split);
			cell->rehash(expanded_cell);

			/*
			 * After rehashing, there is no guarantee that two cells will have
			 * the same number of pages. We need to either add empty pages to
			 * the cell without enough pages or rebalance the two cells.
			 */

			/* Add pages to the cell without enough pages. */
			int num_required = max(expanded_cell->get_num_pages()
				- params.get_SA_min_cell_size(), 0);
			num_required += max(cell->get_num_pages() - params.get_SA_min_cell_size(), 0);
			if (num_required <= npages - pg_idx) {
				/*
				 * Actually only one cell requires more pages, the other
				 * one will just take 0 pages.
				 */
				pg_idx += cell->add_pages_to_min(&pages[pg_idx],
						npages - pg_idx);
				pg_idx += expanded_cell->add_pages_to_min(&pages[pg_idx],
						npages - pg_idx);
			}

			if (expanded_cell->get_num_pages() < params.get_SA_min_cell_size()
					|| cell->get_num_pages() < params.get_SA_min_cell_size()) {
				// If we failed to split a cell, we should merge the two half
				cell->merge(expanded_cell);
				expand_over = true;
				fprintf(stderr, "A cell can't have enough pages, merge back\n");
				break;
			}

			table_lock.write_lock();
			split++;
			table_lock.write_unlock();
		}
		table_lock.write_lock();
		if (split == num_half) {
			split = 0;
			level++;
		}
		table_lock.write_unlock();
	}
	if (pg_idx < npages)
		manager->free_pages(npages - pg_idx, &pages[pg_idx]);
	flags.clear_flag(TABLE_EXPANDING);
	cache_npages.inc(npages);
	return npages - pg_idx;
}

page *associative_cache::search(const page_id_t &pg_id, page_id_t &old_id) {
	/*
	 * search might change the structure of the cell,
	 * and cause the cell table to expand.
	 * Thus, the page might not be placed in the cell
	 * we found before. Therefore, we need to research
	 * for the cell.
	 */
	do {
		// KH: finding set
		page *p = get_cell_offset(pg_id)->search(pg_id, old_id);
#ifdef DEBUG
		if (p->is_old_dirty())
			num_dirty_pages.dec(1);
#endif
		return p;
	} while (true);
}

page *associative_cache::search(const page_id_t &pg_id)
{
	do {
		return get_cell_offset(pg_id)->search(pg_id);
	} while (true);
}

int associative_cache::get_num_used_pages() const
{
	unsigned long count;
	int npages = 0;
	do {
		table_lock.read_lock(count);
		int ncells = get_num_cells();
		for (int i = 0; i < ncells; i++) {
			npages += get_cell(i)->get_num_pages();
		}
	} while (!table_lock.read_unlock(count));
	return npages;
}

void associative_cache::sanity_check() const
{
	unsigned long count;
	do {
		table_lock.read_lock(count);
		int ncells = get_num_cells();
		for (int i = 0; i < ncells; i++) {
			hash_cell *cell = get_cell(i);
			cell->sanity_check();
		}
	} while (!table_lock.read_unlock(count));
}

associative_cache::associative_cache(long cache_size, long max_cache_size,
		int node_id, int offset_factor, int _max_num_pending_flush,
		bool expandable): max_num_pending_flush(_max_num_pending_flush)
{
	this->offset_factor = offset_factor;
	pthread_mutex_init(&init_mutex, NULL);
#ifdef DEBUG
	printf("associative cache is created on node %d, cache size: %ld, min cell size: %d\n",
			node_id, cache_size, params.get_SA_min_cell_size());
#endif
	this->node_id = node_id;
	level = 0;
	split = 0;
	height = params.get_SA_min_cell_size();
	expand_cell_idx = 0;
	this->expandable = expandable;
	this->manager = memory_manager::create(max_cache_size, node_id);
	manager->register_cache(this);
	long init_cache_size = default_init_cache_size;
	if (init_cache_size > cache_size
			// If the cache isn't expandable, let's just use the maximal
			// cache size at the beginning.
			|| !expandable)
		init_cache_size = cache_size;
	int min_cell_size = params.get_SA_min_cell_size();
	if (init_cache_size < min_cell_size * PAGE_SIZE)
		init_cache_size = min_cell_size * PAGE_SIZE;
	int npages = init_cache_size / PAGE_SIZE;
	init_ncells = npages / min_cell_size;

#ifdef USE_APR
	global_score = DEFAULT_POLICY;
	global_policy = DEFAULT_POLICY;
	global_ghost = new ghost_t[get_file_size(APR_FILE_NAME)/PAGE_SIZE+1]();
	unsigned int metasize = get_file_size(APR_META_NAME);
	page_info = new page_info_t[metasize]();
	load_page_info(page_info, metasize);
#endif
	time_init(1, &cache_time);

	// KH: create classes of hash_cell (count = npages/12)
	hash_cell *cells = hash_cell::create_array(node_id, init_ncells);
	int max_npages = manager->get_max_size() / PAGE_SIZE;
	try {
		for (int i = 0; i < init_ncells; i++)
			cells[i].init(this, i, true);
	} catch (oom_exception e) {
		fprintf(stderr,
				"out of memory: max npages: %d, init npages: %d\n",
				max_npages, npages);
		throw e;
	}

	cells_table.push_back(cells);

	int max_ncells = max_npages / min_cell_size;
	for (int i = 1; i < max_ncells / init_ncells; i++)
		cells_table.push_back(NULL);

	if (expandable && cache_size > init_cache_size)
		expand((cache_size - init_cache_size) / PAGE_SIZE);
}

/**
 * This defines an interface for implementing the policy of selecting
 * dirty pages for flushing.
 */
class select_dirty_pages_policy
{
public:
	// It select a specified number of pages from the page set.
	virtual int select(hash_cell *cell, int num_pages,
			std::map<off_t, thread_safe_page *> &pages) = 0;
};

/**
 * The policy selects dirty pages that are most likely to be evicted
 * by the eviction policy.
 */
class eviction_select_dirty_pages_policy: public select_dirty_pages_policy
{
public:
	int select(hash_cell *cell, int num_pages,
			std::map<off_t, thread_safe_page *> &pages) {
		char set_flags = 0;
		char clear_flags = 0;
		page_set_flag(set_flags, DIRTY_BIT, true);
		page_set_flag(clear_flags, IO_PENDING_BIT, true);
		page_set_flag(clear_flags, PREPARE_WRITEBACK, true);
		cell->predict_evicted_pages(num_pages, set_flags, clear_flags, pages);
		return pages.size();
	}
};

/**
 * This policy simply selects some dirty pages in a page set.
 */
class default_select_dirty_pages_policy: public select_dirty_pages_policy
{
public:
	int select(hash_cell *cell, int num_pages,
			std::map<off_t, thread_safe_page *> &pages) {
		char set_flags = 0;
		char clear_flags = 0;
		page_set_flag(set_flags, DIRTY_BIT, true);
		page_set_flag(clear_flags, IO_PENDING_BIT, true);
		page_set_flag(clear_flags, PREPARE_WRITEBACK, true);
		cell->get_pages(num_pages, set_flags, clear_flags, pages);
		return pages.size();
	}
};

class associative_flusher;

class flush_io: public io_interface
{
	io_interface::ptr underlying;
	pthread_key_t underlying_key;
	associative_cache *cache;
	associative_flusher *flusher;

	io_interface *get_per_thread_io() {
		io_interface *io = (io_interface *) pthread_getspecific(underlying_key);
		if (io == NULL) {
			thread *curr = thread::get_curr_thread();
			assert(curr);
			io = underlying->clone(curr);
			pthread_setspecific(underlying_key, io);
		}
		return io;
	}
public:
	flush_io(io_interface::ptr underlying, associative_cache *cache,
			associative_flusher *flusher): io_interface(NULL,
				underlying->get_header()) {
		this->underlying = underlying;
		this->cache = cache;
		this->flusher = flusher;
		pthread_key_create(&underlying_key, NULL);
	}

	virtual int get_file_id() const {
		throw unsupported_exception("get_file_id");
	}

	virtual void notify_completion(io_request *reqs[], int num);
	virtual void access(io_request *requests, int num, io_status *status = NULL) {
		get_per_thread_io()->access(requests, num, status);
	}
	virtual void flush_requests() {
		get_per_thread_io()->flush_requests();
	}
	virtual int wait4complete(int num) {
		throw unsupported_exception("wait4complete");
	}

	virtual void cleanup() {
		throw unsupported_exception("cleanup");
	}
};

class associative_flusher: public dirty_page_flusher
{
	// For the case of NUMA cache, cache and local_cache are different.
	page_cache *cache;
	associative_cache *local_cache;
	int node_id;

	std::unique_ptr<flush_io> io;
	std::unique_ptr<select_dirty_pages_policy> policy;
public:
	thread_safe_FIFO_queue<hash_cell *> dirty_cells;
	associative_flusher(page_cache *cache, associative_cache *local_cache,
			io_interface::ptr io, int node_id): dirty_cells(
				std::string("dirty_cells-") + itoa(io->get_node_id()),
				io->get_node_id(), local_cache->get_num_cells()) {
		this->node_id = node_id;
		this->cache = cache;
		this->local_cache = local_cache;
		if (this->cache == NULL)
			this->cache = local_cache;

		this->io = std::unique_ptr<flush_io>(new flush_io(io, local_cache, this));
		policy = std::unique_ptr<select_dirty_pages_policy>(new eviction_select_dirty_pages_policy());
	}

	int get_node_id() const {
		return node_id;
	}

	void run();
	void flush_dirty_pages(thread_safe_page *pages[], int num,
			io_interface &io);
	int flush_dirty_pages(page_filter *filter, int max_num);
	int flush_cell(hash_cell *cell, io_request *req_array, int req_array_size);
};

void flush_io::notify_completion(io_request *reqs[], int num)
{
	hash_cell *dirty_cells[num];
	int num_dirty_cells = 0;
	int num_flushes = 0;
	for (int i = 0; i < num; i++) {
		// If the request is discarded by the I/O thread, we need to
		// check the page set where it is located.
		// If the page set isn't in the queue of dirty page sets,
		// we need to check if the page set contains pages that
		// we should flush.
		if (reqs[i]->is_discarded()) {
			page_id_t pg_id(reqs[i]->get_file_id(), reqs[i]->get_offset());
			hash_cell *cell = cache->get_cell_offset(pg_id);
#ifdef DEBUG
			assert(cell->contain(reqs[i]->get_page(0)));
#endif
			if (cell->is_in_queue())
				continue;

			// Try to add more flushes only when there aren't many pending
			// flush requests.
			if (cache->num_pending_flush.get() < cache->max_num_pending_flush) {
				io_request req_array[NUM_WRITEBACK_DIRTY_PAGES];
				int ret = flusher->flush_cell(cell, req_array,
						NUM_WRITEBACK_DIRTY_PAGES);
				if (ret > 0) {
					this->access(req_array, ret);
					num_flushes += ret;
				}
				// If we get what we ask for, maybe there are more dirty pages
				// we can flush. Add the dirty cell back in the queue.
				if (ret == NUM_WRITEBACK_DIRTY_PAGES && !cell->set_in_queue(true))
					dirty_cells[num_dirty_cells++] = cell;
			}
			else
				// Let's add the cell for later examination.
				dirty_cells[num_dirty_cells++] = cell;

			continue;
		}

		assert(reqs[i]->get_num_bufs());
		if (reqs[i]->get_num_bufs() == 1) {
			thread_safe_page *p = (thread_safe_page *) reqs[i]->get_page(0);
			p->lock();
			assert(p->is_dirty());
			p->set_dirty(false);
			p->set_io_pending(false);
			BOOST_VERIFY(p->reset_reqs() == NULL);
			p->unlock();
			p->dec_ref();
		}
		else {
			off_t off = reqs[i]->get_offset();
			for (int j = 0; j < reqs[i]->get_num_bufs(); j++) {
				thread_safe_page *p = reqs[i]->get_page(j);
				assert(p);
				p->lock();
				assert(p->is_dirty());
				p->set_dirty(false);
				p->set_io_pending(false);
				BOOST_VERIFY(p->reset_reqs() == NULL);
				p->unlock();
				p->dec_ref();
				assert(p->get_ref() >= 0);
				off += PAGE_SIZE;
			}
		}

		delete reqs[i]->get_extension();
	}
	if (num_dirty_cells > 0)
		flusher->dirty_cells.add(dirty_cells, num_dirty_cells);
	if (num_flushes > 0)
		cache->num_pending_flush.inc(num_flushes);

	cache->num_pending_flush.dec(num);
#ifdef DEBUG
	cache->num_dirty_pages.dec(num);
	int orig = cache->num_pending_flush.get();
#endif
	if (cache->num_pending_flush.get() < cache->max_num_pending_flush) {
		flusher->run();
	}
#ifdef DEBUG
	if (enable_debug)
		printf("node %d: %d orig, %d pending, %d dirty cells, %d dirty pages\n",
				get_node_id(), orig, cache->num_pending_flush.get(),
				flusher->dirty_cells.get_num_entries(),
				cache->num_dirty_pages.get());
#endif
}

void merge_pages2req(io_request &req, page_cache *cache);

int associative_flusher::flush_cell(hash_cell *cell,
		io_request *req_array, int req_array_size)
{
	std::map<off_t, thread_safe_page *> dirty_pages;
	policy->select(cell, NUM_WRITEBACK_DIRTY_PAGES, dirty_pages);
	int num_init_reqs = 0;
	for (std::map<off_t, thread_safe_page *>::const_iterator it
			= dirty_pages.begin(); it != dirty_pages.end(); it++) {
		thread_safe_page *p = it->second;
		p->lock();
		assert(!p->is_old_dirty());
		assert(p->data_ready());

		assert(num_init_reqs < req_array_size);
		// Here we flush dirty pages with normal requests.
#if 0
		if (!p->is_io_pending() && !p->is_prepare_writeback()
				// The page may have been cleaned.
				&& p->is_dirty()) {
			// TODO global_cached_io may delete the extension.
			// I'll fix it later.
			if (!req_array[num_init_reqs].is_extended_req()) {
				io_request tmp(true);
				req_array[num_init_reqs] = tmp;
			}
			req_array[num_init_reqs].init(p->get_offset(), WRITE, io,
					get_node_id(), NULL, cache, NULL);
			req_array[num_init_reqs].add_page(p);
			req_array[num_init_reqs].set_high_prio(true);
			p->set_io_pending(true);
			p->unlock();
			merge_pages2req(req_array[num_init_reqs], cache);
			num_init_reqs++;
		}
		else {
			p->unlock();
			p->dec_ref();
		}
#endif

		// The code blow flush dirty pages with low-priority requests.
		if (!p->is_io_pending() && !p->is_prepare_writeback()
				// The page may have been cleaned.
				&& p->is_dirty()) {
			data_loc_t loc(p->get_file_id(), p->get_offset());
			new (req_array + num_init_reqs) io_request(
					new io_req_extension(), loc, WRITE, io.get(), get_node_id());
			req_array[num_init_reqs].set_priv(cache);
			req_array[num_init_reqs].add_page(p);
			req_array[num_init_reqs].set_high_prio(false);
#ifdef STATISTICS
			req_array[num_init_reqs].set_timestamp();
#endif
			num_init_reqs++;
			p->set_prepare_writeback(true);
		}
		// When a page is put in the queue for writing back,
		// the queue of the IO thread doesn't own the page, which
		// means that the page can be evicted.
		p->unlock();
		p->dec_ref();
	}
	return num_init_reqs;
}

/**
 * This will run until we get enough pending flushes.
 */
void associative_flusher::run()
{
	const int FETCH_BUF_SIZE = 32;
	// We can't get more requests than the number of pages in a cell.
	io_request req_array[NUM_WRITEBACK_DIRTY_PAGES];
	int tot_flushes = 0;
	while (dirty_cells.get_num_entries() > 0) {
		hash_cell *cells[FETCH_BUF_SIZE];
		hash_cell *tmp[FETCH_BUF_SIZE];
		int num_dirty_cells = 0;
		int num_fetches = dirty_cells.fetch(cells, FETCH_BUF_SIZE);
		int num_flushes = 0;
		for (int i = 0; i < num_fetches; i++) {
			int ret = flush_cell(cells[i], req_array,
					NUM_WRITEBACK_DIRTY_PAGES);
			if (ret > 0) {
				io->access(req_array, ret);
				num_flushes += ret;
			}
			// If we get what we ask for, maybe there are more dirty pages
			// we can flush. Add the dirty cell back in the queue.
			if (ret == NUM_WRITEBACK_DIRTY_PAGES)
				tmp[num_dirty_cells++] = cells[i];
			else {
				// We can clear the in_queue flag now.
				// The cell won't be added to the queue for flush until its dirty pages
				// have been written back successfully.
				// A cell is added to the queue only when the number of dirty pages
				// that aren't being written back is larger than a threshold.
				cells[i]->set_in_queue(false);
			}
		}
		dirty_cells.add(tmp, num_dirty_cells);
		local_cache->num_pending_flush.inc(num_flushes);
		tot_flushes += num_flushes;

		// If we have flushed enough pages, we can stop now.
		if (local_cache->num_pending_flush.get()
				> local_cache->max_num_pending_flush) {
			break;
		}
	}
	io->flush_requests();
}

void associative_cache::create_flusher(io_interface::ptr io,
		page_cache *global_cache)
{
	pthread_mutex_lock(&init_mutex);
	if (_flusher == NULL && io
			// The IO instance should be on the same node or we don't know
			// in which node the cache is.
			&& (io->get_node_id() == node_id || node_id == -1)) {
		_flusher = std::unique_ptr<dirty_page_flusher>(
				new associative_flusher(global_cache, this, io, node_id));
	}
	pthread_mutex_unlock(&init_mutex);
}

void associative_cache::mark_dirty_pages(thread_safe_page *pages[], int num,
		io_interface &io)
{
#ifdef DEBUG
	num_dirty_pages.inc(num);
#endif
	if (_flusher)
		_flusher->flush_dirty_pages(pages, num, io);
}

void associative_cache::init(io_interface::ptr underlying)
{
	create_flusher(underlying, this);
}

hash_cell *associative_cache::get_prev_cell(hash_cell *cell) {
	long index = cell->get_hash();
	// The first cell in the hash table.
	if (index == 0)
		return NULL;
	// The cell is in the middle of a cell array.
	if (index % init_ncells)
		return cell - 1;
	else {
		unsigned i;
		for (i = 0; i < cells_table.size(); i++) {
			if (cell == cells_table[i]) {
				assert(i > 0);
				// return the last cell in the previous cell array.
				return (cells_table[i - 1] + init_ncells - 1);
			}
		}
		// we shouldn't reach here if the cell exists in the table.
		return NULL;
	}
}

hash_cell *associative_cache::get_next_cell(hash_cell *cell)
{
	long index = cell->get_hash();
	// If it's not the last cell in the cell array.
	if (index % init_ncells != init_ncells - 1)
		return cell + 1;
	else {
		unsigned i;
		hash_cell *first = cell + 1 - init_ncells;
		for (i = 0; i < cells_table.size(); i++) {
			if (first == cells_table[i]) {
				if (i == cells_table.size() - 1)
					return NULL;
				else
					return cells_table[i + 1];
			}
		}
		// We shouldn't reach here.
		return NULL;
	}
}

int hash_cell::num_pages(char set_flags, char clear_flags)
{
	int num = 0;
	_lock.lock();
	for (unsigned int i = 0; i < buf.get_num_pages(); i++) {
		thread_safe_page *p = buf.get_page(i);
		if (p->test_flags(set_flags) && !p->test_flags(clear_flags))
			num++;
	}
	_lock.unlock();
	return num;
}

void hash_cell::predict_evicted_pages(int num_pages, char set_flags,
		char clear_flags, std::map<off_t, thread_safe_page *> &pages)
{
	_lock.lock();
	policy.predict_evicted_pages(buf, num_pages, set_flags,
			clear_flags, pages);
	bool print = false;
	for (std::map<off_t, thread_safe_page *>::iterator it = pages.begin();
			it != pages.end(); it++) {
		it->second->inc_ref();
		if (it->second->get_flush_score() >= MAX_NUM_WRITEBACK)
			print = true;
	}
	_lock.unlock();

	if (print) {
		for (std::map<off_t, thread_safe_page *>::iterator it = pages.begin();
				it != pages.end(); it++) {
			printf("flush page %lx\n", it->second->get_offset());
		}
		print_cell();
	}
}

void hash_cell::get_pages(int num_pages, char set_flags, char clear_flags,
		std::map<off_t, thread_safe_page *> &pages)
{
	_lock.lock();
	for (int i = 0; i < (int) buf.get_num_pages(); i++) {
		thread_safe_page *p = buf.get_page(i);
		if (p->test_flags(set_flags) && !p->test_flags(clear_flags)) {
			p->inc_ref();
			pages.insert(std::pair<off_t, thread_safe_page *>(
						p->get_offset(), p));
		}
	}
	_lock.unlock();
}

void associative_flusher::flush_dirty_pages(thread_safe_page *pages[],
		int num, io_interface &io)
{
	if (!params.is_use_flusher()) {
		return;
	}

	hash_cell *cells[num];
	int num_queued_cells = 0;
	int num_flushes = 0;
	for (int i = 0; i < num; i++) {
		page_id_t pg_id(pages[i]->get_file_id(), pages[i]->get_offset());
		hash_cell *cell = local_cache->get_cell_offset(pg_id);
		char dirty_flag = 0;
		char skip_flags = 0;
		page_set_flag(dirty_flag, DIRTY_BIT, true);
		// We should skip pages in IO pending or in a writeback queue.
		page_set_flag(skip_flags, IO_PENDING_BIT, true);
		page_set_flag(skip_flags, PREPARE_WRITEBACK, true);
		/*
		 * We only count the number of dirty pages without IO pending.
		 * If a page is dirty but has IO pending, it means the page
		 * is being written back, so we don't need to do anything with it.
		 */
		int n = cell->num_pages(dirty_flag, skip_flags);
		if (n > DIRTY_PAGES_THRESHOLD) {
			if (local_cache->num_pending_flush.get()
					> local_cache->max_num_pending_flush) {
				if (!cell->set_in_queue(true))
					cells[num_queued_cells++] = cell;
			}
			else {
				io_request req_array[NUM_WRITEBACK_DIRTY_PAGES];
				int ret = flush_cell(cell, req_array,
						NUM_WRITEBACK_DIRTY_PAGES);
				io.access(req_array, ret);
				num_flushes += ret;
				// If it has the required number of dirty pages to flush,
				// it may have more to be flushed.
				if (ret == NUM_WRITEBACK_DIRTY_PAGES && n - ret > 6)
					if (!cell->set_in_queue(true))
						cells[num_queued_cells++] = cell;
			}
		}
	}
	if (num_flushes > 0)
		local_cache->num_pending_flush.inc(num_flushes);
	if (num_queued_cells > 0) {
		// TODO currently, there is only one flush thread. Adding dirty cells
		// requires to grab a spin lock. It may not work well on a NUMA machine.
		int ret = dirty_cells.add(cells, num_queued_cells);
		if (ret < num_queued_cells) {
			printf("only queue %d in %d dirty cells\n", ret, num_queued_cells);
		}
	}
#ifdef DEBUG
	if (enable_debug)
		printf("node %d: %d flushes, %d pending, %d dirty cells, %d dirty pages\n",
				get_node_id(), num_flushes, local_cache->num_pending_flush.get(),
				dirty_cells.get_num_entries(), local_cache->num_dirty_pages.get());
#endif
}

int associative_flusher::flush_dirty_pages(page_filter *filter, int max_num)
{
	if (!params.is_use_flusher())
		return 0;

	int num_flushes = 0;

	while (num_flushes < max_num) {
		int num_cells = (max_num - num_flushes) / NUM_WRITEBACK_DIRTY_PAGES;
		if (num_cells == 0)
			num_cells = 1;
		hash_cell *cells[num_cells];
		hash_cell *queue_cells[num_cells];
		int num_queued_cells = 0;
		int num_fetched_cells = dirty_cells.fetch(cells, num_cells);
		if (num_fetched_cells == 0)
			return num_flushes;
		io_request req_array[NUM_WRITEBACK_DIRTY_PAGES];
		for (int i = 0; i < num_fetched_cells; i++) {
			int ret = flush_cell(cells[i], req_array, NUM_WRITEBACK_DIRTY_PAGES);
			io->access(req_array, ret);
			num_flushes += ret;
			if (ret == NUM_WRITEBACK_DIRTY_PAGES)
				queue_cells[num_queued_cells++] = cells[i];
			else
				cells[i]->set_in_queue(false);
		}
		dirty_cells.add(queue_cells, num_queued_cells);
	}
	io->flush_requests();

	int pending = local_cache->num_pending_flush.inc(num_flushes);
	local_cache->recorded_max_num_pending.add(pending);
	local_cache->avg_num_pending.add(pending);

	return num_flushes;
}

int associative_cache::get_num_dirty_pages() const
{
	int num = 0;
	for (int i = 0; i < get_num_cells(); i++) {
		hash_cell *cell = get_cell(i);
		char set_flag = 0;
		page_set_flag(set_flag, DIRTY_BIT, true);
		int n = cell->num_pages(set_flag, 0);
		num += n;
	}
#ifdef DEBUG
	if (num != num_dirty_pages.get())
		printf("the counted dirty pages: %d, there are actually %d dirty pages\n",
				num_dirty_pages.get(), num);
#endif
	return num;
}

int associative_cache::flush_dirty_pages(page_filter *filter, int max_num)
{
	if (_flusher)
		return _flusher->flush_dirty_pages(filter, max_num);
	else
		return 0;
}

}
