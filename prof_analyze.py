#!/usr/bin/env python3
import sys, csv
from collections import defaultdict

def load(path):
    rows=[]
    with open(path) as f:
        r=csv.reader(f); next(r)
        for op,cnt,cyc in r:
            rows.append((int(op,16),int(cnt),int(cyc)))
    return rows

EA_NAMES={0:'Dn',1:'An',2:'(An)',3:'(An)+',4:'-(An)',5:'(d16,An)',6:'(d8,An,Xn)'}
def ea_name(mode,reg):
    if mode<7: return EA_NAMES[mode]
    return {0:'(xxx).w',1:'(xxx).l',2:'(d16,PC)',3:'(d8,PC,Xn)',4:'#imm'}.get(reg,'ea7?')
def ea_is_mem(mode,reg):
    if mode in (0,1): return False        # Dn / An direct
    if mode==7 and reg==4: return False    # immediate
    return True

def classify(op):
    """Return (coarse_class, detail) for a 16-bit opcode."""
    top=op>>12
    if top in (1,2,3):
        sz={1:'b',2:'l',3:'w'}[top]
        dmode=(op>>6)&7; dreg=(op>>9)&7
        smode=(op>>3)&7; sreg=op&7
        if dmode==1:  # MOVEA
            return ('MOVEA.'+sz, 'MOVEA.%s %s->An'%(sz,ea_name(smode,sreg)))
        return ('MOVE.'+sz, 'MOVE.%s %s->%s'%(sz,ea_name(smode,sreg),ea_name(dmode,dreg)))
    if top==7:
        return ('MOVEQ','MOVEQ')
    if top==6:
        if (op&0x0F00)==0x0000: return ('BRA','BRA')
        if (op&0x0F00)==0x0100: return ('BSR','BSR')
        return ('Bcc','Bcc')
    if top==5:
        if (op&0x00F8)==0x00C8: return ('DBcc','DBcc')
        if (op&0x00C0)==0x00C0: return ('Scc','Scc')
        return ('ADDQ/SUBQ','ADDQ/SUBQ %s'%ea_name((op>>3)&7,op&7))
    if top==0:
        if (op&0xFF00)==0x0000: return ('ORI','immediate ORI')
        if (op&0xFF00)==0x0200: return ('ANDI','immediate ANDI')
        if (op&0xFF00)==0x0400: return ('SUBI','immediate SUBI')
        if (op&0xFF00)==0x0600: return ('ADDI','immediate ADDI')
        if (op&0xFF00)==0x0A00: return ('EORI','immediate EORI')
        if (op&0xFF00)==0x0C00: return ('CMPI','immediate CMPI')
        if (op&0xFF00)==0x0800 or (op&0xF100)==0x0100:
            return ('BTST/BCHG/BCLR/BSET','bit op')
        return ('imm/bit misc','0x0 misc')
    if top==4:
        if (op&0xFFC0)==0x4EC0: return ('JMP','JMP')
        if (op&0xFFC0)==0x4E80: return ('JSR','JSR')
        if op==0x4E75: return ('RTS','RTS')
        if op==0x4E77: return ('RTR','RTR')
        if op==0x4E73: return ('RTE','RTE')
        if op==0x4E71: return ('NOP','NOP')
        if (op&0xFFF8)==0x4E50: return ('LINK','LINK')
        if (op&0xFFF8)==0x4E58: return ('UNLK','UNLK')
        if (op&0xF1C0)==0x41C0: return ('LEA','LEA')
        if (op&0xFFC0)==0x4840: return ('PEA/SWAP','PEA/SWAP')
        if (op&0xFB80)==0x4880: return ('MOVEM','MOVEM')
        if (op&0xFF00)==0x4200: return ('CLR','CLR.%s %s'%('bwl'[(op>>6)&3] if ((op>>6)&3)<3 else '?',ea_name((op>>3)&7,op&7)))
        if (op&0xFF00)==0x4A00:
            if (op&0x00C0)==0x00C0: return ('TAS','TAS')
            return ('TST','TST.%s %s'%('bwl'[(op>>6)&3] if ((op>>6)&3)<3 else '?',ea_name((op>>3)&7,op&7)))
        if (op&0xFF00)==0x4400: return ('NEG','NEG')
        if (op&0xFF00)==0x4600: return ('NOT','NOT')
        if (op&0xFF00)==0x4000: return ('NEGX','NEGX')
        if (op&0xFFB8)==0x4880: return ('EXT','EXT')
        if (op&0xFFF0)==0x4E40: return ('TRAP','TRAP')
        return ('4xxx misc','4xxx misc')
    if top in (8,9,0xB,0xC,0xD):
        fam={8:'OR',9:'SUB',0xB:'CMP/EOR',0xC:'AND',0xD:'ADD'}[top]
        opmode=(op>>6)&7; smode=(op>>3)&7; sreg=op&7
        # special sub-encodings
        if top==0xC and (op&0x00F0)==0x0000 and opmode in (4,5): pass
        if top in (0x8,0xC) and opmode==3: return ('MUL/DIV','%s MUL/DIV'%('DIV' if top==8 else 'MUL'))
        if top in (0x8,0xC) and opmode==7: return ('MUL/DIV','%s MUL/DIV.l?'%fam)
        if opmode in (3,7):  # ADDA/SUBA/CMPA (address)
            return (fam+'A','%sA %s->An'%(fam,ea_name(smode,sreg)))
        # ABCD/SBCD/ADDX/SUBX/CMPM/EXG detection (register/mem special)
        if top in (0x8,0xC) and (op&0x01F0)==0x0100: return ('ABCD/SBCD','bcd')
        if top in (0x9,0xD) and opmode in (4,5,6) and smode in (0,1): return ('ADDX/SUBX','addx/subx')
        if top==0xB and opmode in (4,5,6) and smode==1: return ('CMPM','CMPM')
        if top==0xB and opmode in (4,5,6): return ('EOR','EOR Dn->%s'%ea_name(smode,sreg))
        # generic ALU
        if opmode in (0,1,2):  # <ea> op Dn -> Dn
            if not ea_is_mem(smode,sreg):
                return (fam+' reg,reg','%s %s,Dn (reg-direct)'%(fam,ea_name(smode,sreg)))
            return (fam+' <ea>,Dn','%s %s,Dn (mem src)'%(fam,ea_name(smode,sreg)))
        else:  # 4,5,6 : Dn op <ea> -> <ea> (mem dst)
            return (fam+' Dn,<ea>','%s Dn,%s (mem dst)'%(fam,ea_name(smode,sreg)))
    if top==0xE:
        return ('shift/rotate','shift/rotate')
    if top in (0xA,0xF):
        return ('lineA/F','lineA/F')
    return ('other','other')

def report(path,label,total_insns):
    rows=load(path)
    cls_cnt=defaultdict(int); cls_cyc=defaultdict(int)
    det_cnt=defaultdict(int); det_cyc=defaultdict(int)
    raw=[]
    tot_cyc=0
    for op,cnt,cyc in rows:
        c,d=classify(op)
        cyctot=cnt*cyc
        cls_cnt[c]+=cnt; cls_cyc[c]+=cyctot
        det_cnt[d]+=cnt; det_cyc[d]+=cyctot
        tot_cyc+=cyctot
        raw.append((op,cnt,cyc,cyctot))
    N=total_insns
    print('\n================ %s ================'%label)
    print('total instructions=%d  total(est)cycles=%d'%(N,tot_cyc))
    print('\n--- COARSE CLASS histogram (by count) ---')
    print('%-24s %14s %7s %16s %7s'%('class','count','cnt%','cycles','cyc%'))
    for c in sorted(cls_cnt,key=lambda k:-cls_cnt[k]):
        print('%-24s %14d %6.2f%% %16d %6.2f%%'%(c,cls_cnt[c],100*cls_cnt[c]/N,cls_cyc[c],100*cls_cyc[c]/tot_cyc))
    print('\n--- by CYCLE share (top 25 classes) ---')
    for c in sorted(cls_cyc,key=lambda k:-cls_cyc[k])[:25]:
        print('%-24s %16d %6.2f%%   (cnt %6.2f%%)'%(c,cls_cyc[c],100*cls_cyc[c]/tot_cyc,100*cls_cnt[c]/N))
    print('\n--- DETAIL histogram (top 40 by count) ---')
    for d in sorted(det_cnt,key=lambda k:-det_cnt[k])[:40]:
        print('%-34s %14d %6.2f%%  cyc%%=%6.2f%%'%(d,det_cnt[d],100*det_cnt[d]/N,100*det_cyc[d]/tot_cyc))
    print('\n--- TOP 50 RAW opcodes (by count) ---')
    raw.sort(key=lambda x:-x[1])
    for op,cnt,cyc,cyctot in raw[:50]:
        c,d=classify(op)
        print('%04x cnt=%12d %6.3f%% basecyc=%3d cyc%%=%6.3f%%  %s'%(op,cnt,100*cnt/N,cyc,100*cyctot/tot_cyc,d))
    return cls_cnt,cls_cyc,tot_cyc

if __name__=='__main__':
    import os
    os.chdir('/root/N64-NEOGEO/mvs64')
    # totals from summary
    S={}
    for line in open('prof_summary.txt'):
        if '=' in line:
            k,v=line.strip().split('=',1)
            try: S[k]=int(v)
            except ValueError: S[k]=v
    report('prof_op_match.csv','IN-MATCH (frames 2900-5600)',S['n_match'])
    report('prof_op_all.csv','WHOLE RUN (frames 0-5700)',S['n_all'])
    print('\n================ BRANCH OUTCOMES (in-match) ================')
    bt,bn=S['bcc_taken'],S['bcc_nottaken']; dl,df=S['dbcc_looped'],S['dbcc_fell']
    print('Bcc  taken=%d (%.1f%%)  not-taken=%d (%.1f%%)  total=%d (%.2f%% of insns)'%(
        bt,100*bt/(bt+bn),bn,100*bn/(bt+bn),bt+bn,100*(bt+bn)/S['n_match']))
    print('DBcc looped=%d (%.1f%%)  fell-through=%d (%.1f%%)  total=%d (%.2f%% of insns)'%(
        dl,100*dl/(dl+df),df,100*df/(dl+df),dl+df,100*(dl+df)/S['n_match']))
