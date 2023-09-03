# Loongarch64 LibFIRM TODO List

- [x] LA64 Spec File
  - [x] Arithmetic & Bit-shifting Operations
  - [x] Bit-manipulation Instructions (FIRM IR doesn't support those detailed bit-manipulation operations)
  - [x] Branch Instructions (Need more information about LA64 calling convention)
  - [x] Memory Access
  - [ ] Atomic, Barrier, Others...
- [ ] IR Graph Transformation
  - [x] Arithmetic & Bit-shifting Operations: Add, And, Const, Conv, Div, Eor, Mulh, Mul, Minus, Mod, Not, Or, Shl, SHr, Shrs, Sub
  - [ ] Memory Access Related: Address, Load, Member, Store
  - [ ] Conditional: Cmp, Cond, Mux
  - [ ] Control Flow: Call, IJmp, Jmp, Return, Switch
  - [ ] Others: ASM, Builtin, Phi, Start, Unknown
  - [ ] Projection
- [ ] Instruction Emitter
- [ ] Register Allocation

## Stage

- [x] Compile basic arithemetic program (arithmetic)
- [ ] Compile basic control flow program (conditional/compare/branch)
- [ ] Compile program with global variables (load/store/memory access)
- [ ] Compile multi-function program (call convention)
- [ ] Compile with standard library (call convention)
- [ ] Link & Assemble

## Notes

### Choose right postfix

- `int`: 32bit. `long` `long long`: 64bit.
- `.w` will sign extend to GRLEN
- If no `unsigned` instruction provided, use `signed` rules.
- Basic type conversion:
  - `unsigned long` > `long` > `unsigned int` > `int` > `unsigned short` > `short` > `unsigned char` > `char`
  - And for LA64, we should maintain that the general registers are sign-extended from 32-bits result.
  - Currently I should focus on `int` and `long` only.
  - `int` -> `long`/`unsigned long`: signed extension (no operation)
  - `unsigned int` -> `long`/`unsigned long`: zero extension (`bstrpick`)
  - `long`/`unsigned long` -> `int`/`unsigned int`: signed extension (`slli.w`)
- For `add` `sub` `mul`
  - If result is `int`, use `.w`.
  - If operands are both `int`, use `.w`.
  - Else, use `.d`.
- For `div` `mod`
  - Unlike instructions above, int result of `div` and `mod` is affected by higher bits.
  - Operands: Upcast to the "max" type
  - Operator: `.w` `.wu` `.d` `.du`
  - Result: Downcast to the "result" type
- For `sll` `srl` `sra` `rota`
  - Irrelevant to second operand
  - `int -> int/long`: `.w`
  - `long -> int`: `.d` then sign-extend
  - `long -> long`: `.d`
  - Right shift: `unsigned -> *`: `srl`, `signed -> *`: `sra`
- For `and` `or` `xor`
  - `* -> int`: sign-extend

## Calling Convention

*Learning from `mips_cconv.c`*

### `mips_determine_calling_convention`

Determine Calling Convention `cconv` from function type `ir_type *const fun_type`.

1. Determine the location (`reg` or `memory`) and offsets of parameters and results.
2. Setup reg and offset(only arguments) for `mips_reg_or_slot_t`. It seems that all parameters are assigned the offset.

### `mips_layout_parameter_entities`

1. `be_collect_parameter_entities`
2. Setup parameter entity. If reg param, recreate/or re-assign offset of entity.

### How to pass the parameters: `gen_Call`

1. Get `callee` function entiry(`jal`) or function pointer(`jalr`).
2. Get parameter stack size and `be_new_IncSP` to increase stack size.
3. For every parameter, extend first. If is register, store in `ins` array. If is stored in stack memory, generate a new `sw` node.
4. How to handle caller-saved registers?

### How to get the return value: `gen_Proj_Proj_Call`

1. Projection `pn_Call_T_result` Call -> Tuple of return values. Projection of Tuple is the return value.
2. So it is only valid to use double-Projection to get the return value.
3. Use `be_get_out_for_reg`.

### How to get the parameters: `gen_Proj_Proj_Start`

1. Double-projection is same to `gen_Proj_Proj_Call`.
2. If parameter is on register, use `be_get_Start_proj`. If parameter is on stack, use `new_bd_mips_lw`.

### How to return values: `gen_Return`

1. Setup `mem` `stack` `addr`
2. Setup results
3. Setup callee-saved registers
4. `be_stack_record_chain`

## Specical Instructions

- Call: `bl <func>` `bl %plt(<lib>)`
- Return: `jr $r1` `jr $ra`
- Load local address: `la.local rd, label`