import sys, pefile, capstone
pe = pefile.PE(sys.argv[1], fast_load=True); img = pe.get_memory_mapped_image()
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
for a in sys.argv[2:]:
    rva, n = a.split(':'); rva = int(rva, 16); n = int(n)
    print(f'--- {rva:#x}')
    for i, ins in enumerate(md.disasm(img[rva:rva+400], rva)):
        if i >= n: break
        print(f'{ins.address:#x} {ins.mnemonic} {ins.op_str}')
