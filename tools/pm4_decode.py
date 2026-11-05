"""Decode a raw big-endian PM4 dump (from --ng2_ib_census). Usage: python -I tools/pm4_decode.py file.bin [max]"""
import struct, sys
OPS = {0x10:"NOP",0x21:"REG_RMW",0x22:"DRAW_INDX",0x25:"VIZ_QUERY",0x27:"IM_LOAD",0x2B:"IM_LOAD_IMM",0x2D:"SET_CONSTANT",
       0x2F:"LOAD_ALU_CONST",0x34:"DRAW_INDX_BIN",0x35:"DRAW_INDX_2_BIN",0x36:"DRAW_INDX_2",0x3B:"INVALIDATE_STATE",
       0x3C:"WAIT_REG_MEM",0x3D:"MEM_WRITE",0x3E:"REG_TO_MEM",0x3F:"INDIRECT_BUFFER",0x45:"COND_WRITE",0x46:"EVENT_WRITE",
       0x48:"ME_INIT",0x4A:"FIX_2_FLT_REG",0x50:"SET_BIN_MASK_LO",0x51:"SET_BIN_MASK_HI",0x52:"SET_BIN_SELECT_LO",
       0x53:"SET_BIN_SELECT_HI",0x54:"INTERRUPT",0x55:"SET_CONSTANT2",0x56:"SET_SHADER_CONSTANTS",0x57:"EVENT_WRITE_SHD",
       0x58:"EVENT_WRITE_CFL",0x59:"EVENT_WRITE_EXT",0x5A:"SET_BIN_MASK",0x5B:"EVENT_WRITE_ZPD",0x5E:"CONTEXT_UPDATE",
       0x60:"SET_BIN_SELECT",0x64:"XE_SWAP",0x4C:"WAIT_FOR_IDLE",0x26:"WAIT_FOR_IDLE?"}
data = open(sys.argv[1], "rb").read()
w = struct.unpack(">%dI" % (len(data) // 4), data)
mx = int(sys.argv[2]) if len(sys.argv) > 2 else 10**9
i = 0; n = 0
while i < len(w) and n < mx:
    h = w[i]; t = h >> 30
    if t == 0:
        cnt = ((h >> 16) & 0x3FFF) + 1; reg = h & 0x7FFF
        print(f"{i:5X} T0 reg {reg:04X} x{cnt}: " + " ".join(f"{x:08X}" for x in w[i+1:i+1+min(cnt,8)]))
        i += 1 + cnt
    elif t == 1:
        print(f"{i:5X} T1 {h & 0x7FF:04X}/{(h>>11)&0x7FF:04X}: {w[i+1]:08X} {w[i+2]:08X}"); i += 3
    elif t == 2:
        print(f"{i:5X} T2 nop"); i += 1
    else:
        op = (h >> 8) & 0x7F; cnt = ((h >> 16) & 0x3FFF) + 1
        body = w[i+1:i+1+cnt]
        extra = ""
        if op == 0x2D and body:
            extra = f" [type {body[0]>>16} idx {body[0]&0x7FF:03X}]"
        print(f"{i:5X} {OPS.get(op, hex(op))}{' P' if h & 1 else ''} x{cnt}{extra}: " + " ".join(f"{x:08X}" for x in body[:8]))
        i += 1 + cnt
    n += 1
