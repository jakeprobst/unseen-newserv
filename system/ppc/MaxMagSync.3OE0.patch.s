entry_ptr:
reloc0:
    .offsetof start

start:
    mflr r7

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

    # r5 contains the address of the player's character struct
    lwzx r5, [r3 + r4]

    # 0xd08 is the pointer to the equipped mag
    # r6 contains pointer to equipped mag
    lwz r6, [r5 + 0xd08]

    # skip if mag is null
    cmpwi r6, 0
    beq skip

    # 120 is max mag syncro
    li r3, 120
    # 0x19a is the syncro offset
    sth [r6 + 0x19a], r3

skip:
    mtlr r7
    blr
