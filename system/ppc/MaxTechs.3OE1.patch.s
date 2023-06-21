entry_ptr:
reloc0:
    .offsetof start

start:
    mflr r12
    bl run

# variables passed in from server
tech1:    # 0x00
    .zero
tech2:    # 0x04
    .zero
tech3: # 0x08
    .zero
tech4:  # 0x0C
    .zero
tech5: # 0x10
    .zero

run:
    # r11: address of start of variable table
    mflr r11

    # player array index
    lis r3, 0x805c
    ori r3, r3, 0xbb80

    # r4 contains index into player array
    lwz r4, [r3]

    # multiply by 4 to get the byte offset in player array
    mulli r4, r4, 4

    # pointer to player array
    lis r3, 0x8050
    ori r3, r3, 0x8a80

    # r5 contains the address of the player's character struct
    lwzx r5, [r3 + r4]

    # add tech offset to character address
    addi r5, r5, 0x470

    # loop iterator
    li r3, 0
apply_loop:
    lwzx r6, [r11 + r3]
    stwx [r5 + r3], r6

    addi r3, r3, 4
    cmpwi r3, 0x14
    blt apply_loop

    mtlr r12
    blr


