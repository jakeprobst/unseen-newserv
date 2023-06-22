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

    # add player offset to player array
    add r3, r3, r4

    # r5 contains the address of the player's character struct
    lwz r5, [r3]

    # r6 contains a pointer to the player stat table
    lwz r6, [r5 + 0x2b0]

    # this will be a larger number than the max exp
    lis r3, 0x0FFF
    stw [r6 + 0x1c], r3

    mtlr r7
    blr
