import sys; sys.path.insert(0, r'.')
import relib
W=relib.Img(r'G:\SteamLibrary\steamapps\common\Wo Long 2 Wings of Ember Alpha Demo\WoLong2.exe.unpacked.exe')
N=relib.Img(r'G:\SteamLibrary\steamapps\common\Nioh3\Nioh3.exe.unpacked.exe')
def best(im,a,name):
    for n in (16,20,24,28,32,40,48,64):
        p=im.aob(a,n); h=im.aob_search(p,limit=3)
        if len(h)==1 and h[0]==a:
            print(f'{name} @{a:#x} unique n={n}: {p}'); return
    print(f'{name} @{a:#x} NOT unique: {im.aob(a,48)}')
for a,n in [(0x5d6cd0,'WL2 FrameTiming_Update'),(0x5d6da7,'WL2 latch fps'),(0x5d6eff,'WL2 fixed-step flag check'),(0x5d6f3c,'WL2 store dt'),(0x158f6b0,'WL2 GetUpdatingFps impl'),(0x15d6b90,'WL2 GetFrameStep impl'),(0x5c94bf,'WL2 mainloop call'),(0xd4a3d9,'WL2 fps mode setter tail')]: best(W,a,n)
for a,n in [(0x5ad5f8,'N3 GameTime_Update'),(0x5ad66a,'N3 latch fps'),(0x5ad6a2,'N3 speed copy (patch site)'),(0x5ad6f8,'N3 60Hz accumulator'),(0x5af604,'N3 GetUpdateFps table'),(0x46484,'N3 GetTimeStep'),(0xfba838,'N3 mode-from-refresh'),(0x1fde480,'N3 SetFpsMode'),(0x5acee8,'N3 task update thunk')]: best(N,a,n)
