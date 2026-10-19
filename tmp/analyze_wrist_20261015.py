from pathlib import Path
import json
import numpy as np
import pandas as pd
from scipy import signal
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

OUT=Path(__file__).parent/'wrist_analysis_20261015'
OUT.mkdir(exist_ok=True)
SRC=Path(r'E:\PWV\Project\上位机\records\physio_20261015_101527_107(1).csv')
df=pd.read_csv(SRC,skiprows=1)
fs=250
x=df.iloc[:,1:4].to_numpy(float)
t=np.arange(len(x))/fs
ts=pd.to_datetime(df.iloc[:,0]); dt=ts.diff().dt.total_seconds().to_numpy()[1:]
print('rows',len(x),'duration_sample',t[-1],'wall_duration',(ts.iloc[-1]-ts.iloc[0]).total_seconds(),'missing',df.isna().sum().to_dict())
print('timestamp delta quantiles',np.quantile(dt,[0,.5,.9,.99,1]),'nonzero',np.count_nonzero(dt),'backwards',sum(dt<0))
names=['ECG','Finger PPG','Wrist PPG']
low=signal.sosfiltfilt(signal.butter(4,.5,fs=fs,output='sos'),x,axis=0)
bp=signal.sosfiltfilt(signal.butter(4,[.65,8],btype='bandpass',fs=fs,output='sos'),x,axis=0)
f,p=signal.welch(x,fs,nperseg=min(len(x),250*120),axis=0)
for k,n in enumerate(names):
    print(n,'range',np.min(x[:,k]),np.max(x[:,k]),'std',np.std(x[:,k]),'lowstd',np.std(low[2500:-2500,k]),'bpstd',np.std(bp[2500:-2500,k]))
    for lo,hi in [(.015,.6),(.65,3),(3,60)]:
        inds=np.where((f>=lo)&(f<=hi))[0]; peaks,_=signal.find_peaks(p[inds,k]); inds=inds[peaks]; inds=inds[np.argsort(p[inds,k])[::-1]][:7]
        print(' peaks',lo,hi,[(round(float(f[i]),5),round(float(p[i,k]),1)) for i in inds])
print('lowcorr',np.corrcoef(low[2500:-2500].T))
plt.rcParams.update({'font.family':'Microsoft YaHei','axes.unicode_minus':False,'font.size':10})
fig,axs=plt.subplots(4,1,figsize=(14,10),layout='constrained')
for k in range(3):
    axs[k].plot(t[::10],x[::10,k],lw=.6,color=['#b74b54','#3977b8','#1b9279'][k]);axs[k].plot(t[::10],low[::10,k],color='black',lw=.8,label='<0.5 Hz baseline')
    axs[k].set_ylabel(names[k]+' (codes)');axs[k].legend(loc='upper right');axs[k].grid(alpha=.2)
for k in range(3): axs[3].semilogy(f,p[:,k],label=names[k])
axs[3].set_xlim(0,3);axs[3].set_xlabel('Frequency (Hz)');axs[3].set_ylabel('PSD');axs[3].legend();axs[3].grid(alpha=.2)
fig.savefig(OUT/'overview.png',dpi=150);plt.close(fig)
fig,axs=plt.subplots(3,1,figsize=(14,8),layout='constrained')
for k in range(3):
    m=(t>=40)&(t<100); axs[k].plot(t[m],x[m,k],lw=.8);axs[k].plot(t[m],low[m,k],lw=1.2,color='orange');axs[k].set_ylabel(names[k]);axs[k].grid(alpha=.2)
axs[-1].set_xlabel('Sample time (s; 250 Hz assumed)');fig.savefig(OUT/'detail.png',dpi=150);plt.close(fig)
np.savez(OUT/'analysis.npz',x=x,t=t,low=low,bp=bp,f=f,p=p)

# ECG-locked cardiac template, used only to inspect non-cardiac structure.
r,_=signal.find_peaks(signal.sosfiltfilt(signal.butter(3,[5,25],btype='bandpass',fs=fs,output='sos'),x[:,0]),distance=.65*fs,prominence=20000)
rr=np.diff(r)/fs
print('ECG beats',len(r),'RR quantiles',np.quantile(rr,[0,.1,.5,.9,1]))
phase=np.interp(np.arange(len(x)),r,np.arange(len(r)))%1
bins=np.minimum((phase*200).astype(int),199)
template=np.array([np.median(x[(bins==b)&(t>10)&(t<t[-1]-10),2]) for b in range(200)])
res=x[:,2]-template[bins]
fr,pr=signal.welch(res,fs,nperseg=250*120)
inds,_=signal.find_peaks(pr);inds=inds[(fr[inds]>.02)&(fr[inds]<5)];inds=inds[np.argsort(pr[inds])[::-1]][:15]
print('residual spectral peaks',[(round(float(fr[i]),5),round(float(pr[i]),1)) for i in inds])
fig,axs=plt.subplots(4,1,figsize=(14,10),layout='constrained')
for k in range(3):
    m=(t>=45)&(t<57);axs[k].plot(t[m],x[m,k],lw=1);axs[k].set_ylabel(names[k]);axs[k].grid(alpha=.2)
    for tt in r/fs:
        if 45<tt<57: axs[k].axvline(tt,color='gray',alpha=.3)
axs[3].plot(t[m],res[m]);axs[3].set_ylabel('Wrist minus ECG template');axs[3].grid(alpha=.2)
axs[-1].set_xlabel('Time (s)');fig.savefig(OUT/'cardiac_detail.png',dpi=150);plt.close(fig)
fig,axs=plt.subplots(2,1,figsize=(14,8),layout='constrained')
for k in [1,2]:
    ff,tt,sp=signal.spectrogram(x[:,k],fs,nperseg=250*40,noverlap=250*35)
    ax=axs[k-1];mesh=ax.pcolormesh(tt,ff,10*np.log10(sp+1),shading='auto',vmin=40,vmax=100);ax.set_ylim(0,2);ax.set_ylabel(names[k]+' Hz');fig.colorbar(mesh,ax=ax,label='PSD (dB codes²/Hz)')
axs[-1].set_xlabel('Time (s)');fig.savefig(OUT/'spectrogram.png',dpi=150);plt.close(fig)

edges,_=signal.find_peaks(np.diff(x[:,2]),height=6000,distance=175)
widths=[]
for e in edges:
    if e+60>=len(x)-1: continue
    end=e+10+np.argmin(np.diff(x[:,2])[e+10:e+60])
    if np.diff(x[:,2])[end]<-6000: widths.append((end-e)/fs)
print('sharp edge count',len(edges),'paired widths n',len(widths),'quantiles',np.quantile(widths,[.1,.25,.5,.75,.9]))
fig,axs=plt.subplots(3,1,figsize=(13,9),layout='constrained')
m=(t>=45)&(t<57)
axs[0].plot(t[m],x[m,2],color='#1b9279',lw=1,label='腕部 PPG')
for e in edges:
    if 45<e/fs<57:axs[0].axvline(e/fs,color='#cf5944',alpha=.6,ls='--')
axs[0].set_title('腕部叠加短脉冲：虚线标出部分陡峭上升沿（非完整噪声分离）')
axs[0].set_ylabel('ADC 码值');axs[0].legend();axs[0].set_xlabel('相对记录起点 / 秒')
m=(t>=45.8)&(t<46.35)
axs[1].plot(t[m],x[m,2],color='#1b9279',marker='.',ms=3,lw=1)
axs[1].axvspan(45.972,46.096,color='#e4a64b',alpha=.2)
axs[1].annotate('陡沿间隔约 124 ms\n腕部单点变化约 8,972 码 / 4 ms',xy=(46.02,-62000),xytext=(46.10,-56000),arrowprops={'arrowstyle':'->'},fontsize=10)
axs[1].set_title('单个短脉冲放大：快速抬升、短平台、快速回落');axs[1].set_ylabel('ADC 码值');axs[1].set_xlabel('相对记录起点 / 秒')
for k in [1,2]:
    q=(f>=.015)&(f<=.6);axs[2].semilogy(f[q],p[q,k],label=['','手指 PPG','腕部 PPG'][k])
axs[2].axvline(.05,color='gray',ls='--',alpha=.6)
axs[2].set_title('低频谱：腕部在约 0.05 Hz（20 秒尺度）存在明显成分，频率本身不能确定来源')
axs[2].set_xlabel('频率 / Hz');axs[2].set_ylabel('PSD / 码值²/Hz');axs[2].legend()
for ax in axs:ax.grid(alpha=.2)
fig.savefig(OUT/'findings.png',dpi=160);plt.close(fig)
