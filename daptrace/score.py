import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import pandas as pd
import numpy as np

name =

df = pd.read_csv(name+'.log')
y = df.values
x = df.index
plt.scatter(x, y)
#plt.scatter(x, y, c=np.sign(y), cmap='bwr')
#plt.xlabel('Cache Miss Events')
#plt.ylabel('Score distribution')

plt.xlabel('Miss events')
plt.ylabel('Global score')
plt.title(name)
plt.savefig(name+'.jpg')
