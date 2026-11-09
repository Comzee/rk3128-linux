#!/usr/bin/env python3
"""Plot only the synthetic first-principles record: plot.py RESULTS OUTPUT.png."""
import sys
from pathlib import Path
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.patches import Rectangle

root=Path(sys.argv[1])/'results'
fig,axes=plt.subplots(2,4,figsize=(12,5.6),constrained_layout=True)
for r in range(4):
    base=np.frombuffer((root/f'base-{r}-1-0.raw').read_bytes(),dtype=np.uint8).reshape(64,64,4)
    impulse=np.frombuffer((root/f'impulse-{r}-1-0-32-21.raw').read_bytes(),dtype=np.uint8).reshape(64,64,4)
    axes[0,r].step(np.arange(36),base[:36,32,0],where='mid')
    axes[0,r].set(title=f'Radius code {r}',xlabel='Output row',ylim=(-5,140),xticks=(0,7,14,21,28,35))
    axes[0,r].grid(alpha=.2)
    delta=impulse[:,:,0].astype(int)-base[:,:,0].astype(int)
    im=axes[1,r].imshow(delta[16:27,27:38],vmin=-1,vmax=1,cmap='bwr',origin='upper',extent=(-5.5,5.5,5.5,-5.5))
    axes[1,r].plot(0,0,'rx',markersize=5)
    k=2*r+3
    axes[1,r].add_patch(Rectangle((-k/2,-k/2),k,k,fill=False,ls='--',lw=.8,color='gray'))
    axes[1,r].set(xlabel='Output x − impulse x',xticks=(-4,-2,0,2,4),yticks=(-4,-2,0,2,4))
axes[0,0].set_ylabel('DC output (input128)')
axes[1,0].set_ylabel('Output y − impulse y')
fig.colorbar(im,ax=axes[1,:],label='Output change from matched DC baseline',ticks=(-1,0,1),shrink=.7)
fig.suptitle('RK3128 IEP: uniform positive tables, alpha0; gray124 impulse on gray128\nRed × = input position; dashed square = nominal footprint')
fig.savefig(sys.argv[2],dpi=160)
