// Input: v4/v5 are XOR operands; v6/v7 are unsigned addition operands.
// Output: v10 is XOR; v11 is addition modulo 2^32.
  v_xor_b32 v10, v4, v5
  v_add_nc_u32 v11, v6, v7
