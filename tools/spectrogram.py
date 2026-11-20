#!/usr/bin/env python3
"""Spectrogram PNG of a direct-stream capture (tools/record_band.sh): time runs down, frequency across, the whole 192 kHz window.
  tools/spectrogram.py CAPTURE.wav out.png DIAL_MHZ     (un-mirrors and removes the 9 kHz DDS offset like tools/compare_ft8.py; first 60 s)
Real CW shows as short dashes on a thin vertical line, PSK31 as a steady narrow line with fuzzy edges, RTTY as two lines 170 Hz apart, FT8 as 50 Hz wide blocks every 15 s."""
import numpy as np, sys
from PIL import Image
f,out,dial=sys.argv[1],sys.argv[2],float(sys.argv[3])
raw=np.memmap(f,dtype='<i4',mode='r',offset=44).reshape(-1,2); fs=192000
n=min(len(raw),60*fs); z=(raw[:n,0].astype(np.float64)+1j*raw[:n,1])/2**31; z=np.conj(z)*np.exp(-2j*np.pi*9000*np.arange(n)/fs)
N=16384; nfr=n//N; w=np.hanning(N); P=np.zeros((nfr,N),np.float32)
for i in range(nfr): P[i]=np.abs(np.fft.fftshift(np.fft.fft(z[i*N:(i+1)*N]*w)))**2
db=10*np.log10(P+1e-20); lo=np.percentile(db,5); hi=np.percentile(db,99.8)
img=(np.clip((db-lo)/(hi-lo),0,1)*255).astype(np.uint8); Image.fromarray(img).resize((1800,nfr*2),Image.NEAREST).save(out)
print(out,"window %.3f..%.3f MHz, %.0f s"%(dial-0.096,dial+0.096,nfr*N/fs))
