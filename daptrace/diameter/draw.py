import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import pandas as pd

cap=input("memsize(GB): ")
name='diameter_'+str(cap)+'G'

df = pd.read_csv(name+'.log')
y = df.values
x = df.index
plt.scatter(x, y)
plt.xlabel('Cache Miss Events')
plt.ylabel('Evicted Byte Offset')
plt.title(name)
plt.savefig(name+'.jpg')
