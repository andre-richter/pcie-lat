#!/usr/bin/python

import sys
import numpy as np
import matplotlib.mlab as mlab
import matplotlib.pyplot as plt

y = np.loadtxt(sys.argv[1], delimiter=',')
y = [row[1] for row in y]
ymin = min(y)
ymax = max(y)

n, bins, patches = plt.hist(y, ymax-ymin, normed=1, facecolor='#0065BD')
plt.xlabel('nanoseconds')
plt.ylabel('Probability')
plt.title('Histogram of PCIe latencies (%s samples)' % len(y))

plt.show()
