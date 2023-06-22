entry_ptr:
reloc0:
    .offsetof start

start:
    mflr r12
    bl run

# variables passed in from server
hp:    # 0x00
    .zero
tp:    # 0x04
    .zero
power: # 0x08
    .zero
mind:  # 0x0C
    .zero
evade: # 0x10
    .zero
def:   # 0x14
    .zero
luck:  # 0x18
    .zero
atp:   # 0x1C
    .zero
mst:   # 0x20
    .zero
evp:   # 0x24
    .zero
dfp:   # 0x28
    .zero
lck:   # 0x2C
    .zero

run:
    # r11: address of start of variable table
    mflr r11

    # player array index
    lis r3, 0x805c
    ori r3, r3, 0x4ba0

    # r4 contains index into player array
    lwz r4, [r3]

    # multiply by 4 to get the byte offset in player array
    mulli r4, r4, 4

    # pointer to player array
    lis r3, 0x8050
    ori r3, r3, 0x9b50

    # add player offset to player array
    add r3, r3, r4

    # r6 contains the address of the player's character struct
    lwz r6, [r3]

    # hp
    lwz r8, [r11]
    mulli r8, r8, 2 # gotta 2x the hp mat value
    sth [r6 + 0x2bc], r8

    # tp
    lwz r8, [r11 + 0x04]
    mulli r8, r8, 2 # gotta 2x the tp mat value
    sth [r6 + 0x2be], r8

    # mat table is ordered  u8:  atp mst evp    dfp     lck
    # stat table is ordered u16: atp mst evp hp dfp ata lck
    li r3, 0 # iter counter
apply_loop:
    mulli r4, r3, 4 # variables are 4 bytes wide
    addi r4, r4, 0x08 # power mat starts at 0x08

    lwzx r8, [r11 + r4] # r8: material count

    li r5, 0x374 # material table offset
    add r5, r5, r3 # materials are 1 byte
    stbx [r6 + r5], r8

    addi r4, r4, 0x14 # pow - atp = 0x14
    lwzx r8, [r11 + r4] # stat value

    li r5, 0xd38 # stat table offset
    mulli r4, r3, 2 # stats are 2 bytes

    # due to offsets not matching between mat table and stat table
    # we do some extra math to line them up
    cmpwi r3, 3
    blt dont_need_extra_offset
    addi r4, r4, 2
    cmpwi r3, 4
    blt dont_need_extra_offset
    addi r4, r4, 2

dont_need_extra_offset:
    add r5, r5, r4
    sthx [r6 + r5], r8

    addi r3, r3, 1
    cmpwi r3, 5
    blt apply_loop

    mtlr r12
    blr


