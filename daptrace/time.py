import pandas as pd

testname = 'lifo_time'
thread = '16'

algo = 'wcc'
print('WCC')
cap = [ '128M', '2.6G', '5.2G', '7.8G', '10.4G', '13G', '15.6G', '18.2G', '20.8G', '23.4G', '26G' ]
for x in cap :
    name= testname + '/' + algo + '/' + algo + '_' + x + '_th' + thread + '.log'
    print(x)
    with open(name, 'rt') as f:
        data = f.readlines()
    for line in data:
        if line.__contains__('WCC takes'):
            print(line)
    for line in data:
        if line.__contains__('total(ms)'):
            print(line)


algo = 'scc'
print('SCC')
for x in cap :
    name= testname + '/' + algo + '/' + algo + '_' + x + '_th' + thread + '.log'
    print(x)
    with open(name, 'rt') as f:
        data = f.readlines()
    for line in data:
        if line.__contains__('scc takes'):
            print(line)
    for line in data:
        if line.__contains__('total(ms)'):
            print(line)

algo = 'diameter'
print('Diameter')
for x in cap :
    name= testname + '/' + algo + '/' + algo + '_' + x + '_th' + thread + '.log'
    print(x)
    with open(name, 'rt') as f:
        data = f.readlines()
    for line in data:
        if line.__contains__('seconds in total'):
            print(line)
    for line in data:
        if line.__contains__('total(ms)'):
            print(line)

algo = 'pagerank2'
print('pagerank2')
cap = [ '128M', '1.3G', '2.6G', '3.9G', '5.2G', '6.5G', '7.8G', '9.1G', '10.4G', '11.7G', '13G' ]
for x in cap :
    name= testname + '/' + algo + '/' + algo + '_' + x + '_th' + thread + '.log'
    print(x)
    with open(name, 'rt') as f:
        data = f.readlines()
    for line in data:
        if line.__contains__('seconds in total'):
            print(line)
    for line in data:
        if line.__contains__('total(ms)'):
            print(line)


algo = 'cycle_triangle'
print('Cycle_triangle')
for x in cap :
    if x == '128M':
        continue
    print(x)
    name= testname + '/' + algo + '/' + algo + '_' + x + '_th16.log'
    with open(name, 'rt') as f:
        data = f.readlines()
    for line in data:
        if line.__contains__('seconds to count all triangles'):
            print(line)
    for line in data:
        if line.__contains__('total(ms)'):
            print(line)
