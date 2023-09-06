# libFIRM for Loongarch64 移植报告

*清华大学 计06 徐晨曦*

[TOC]

## libFIRM与FIRM IR简介

### libFIRM

libFIRM定义了一种基于图的SSA中间表示IR，并提供了大量配套的解析、优化、可视化、后端代码生成的功能。主要特色有：

1. 完全基于图的、与源代码和目标代码独立的、SSA形式的中间表示IR语言
2. 与cparser配套，提供了与GCC命令格式兼容的完整C99特性支持
3. 提供了大量分析、优化、寄存器分配算法
4. 目前对x86(ia32)和SPARC提供了成熟的后端支持
5. 后端架构比较解耦，让编写新架构的后端相对容易

### FIRM IR

首先基础数值操作是很直观的，如下面的`1 + 2`：

![](https://pp.ipd.kit.edu/firm/simpleadd.svg)

每个节点上面都有自己的名字和**模式**，其中模式代表了这个节点的结果类型，比如上面的`Is`代表的是`int`，即有符号32位整数。类似的有`Iu` `Ls` `Lu`。如果有节点有多个结果，比如`Cond`节点作为分支节点，有两个输出的执行流。FIRM IR的解决方案是**元组**，即`Tuple`，对应的模式是`mode_T`，然后配合`Proj`节点提取元组对应位置的结果。比如`x * x - 2`：

![](https://pp.ipd.kit.edu/firm/load_mul.svg)

但是图作为稀疏数据结构，如果只根据节点的结果进行分析，很容易产生内存一致性问题。比如：

```c
int main() {
  int a[10];
  a[0] = 1;
  return a[0];
}
```

我们发现实际上`Store`节点不会产生结果，而`Return`依赖的结果是`Load`产生的，这种图结果会导致认为`Store`节点是不需要的或者执行顺序错误。为了解决内存一致性问题，我们引入`mode_M`模式的结果指示内存依赖，这样就可以强制保证`Load`一定在`Store`的后面执行。

在进一步介绍控制流之前，我们引入基本块的概念。和其他地方的概念相同，一个块内的节点基本上均是线性流程的。如一下代码：

```c
if (1 == 0) {
        return 4;
} else {
        return 7;
}
```

会产生：

![](https://pp.ipd.kit.edu/firm/condjump.svg)

可以看到`Cond`被`Proj`到了不同的分支上，描述程序执行流的模式为`mode_X`。

由于SSA的约束，同一个变量只能被赋值一次，这就导致在不同分支中被赋值的同一个变量对于IR图来说是不一样的。为了在分支结束后继续使用，我们引入`Phi`节点用于聚合不同的变量，如：

```c
int x;
if (7 > 4) {
        x = 9;
} else {
        x = 3;
}
return x;
```

![](https://pp.ipd.kit.edu/firm/phi.svg)

最后我们来介绍函数定义、调用相关的结构。一个函数由`Start`开始，由`End`结束。这是因为一个函数可能有若干个`Return`，但是只能有一个出口点，因此采用`End`聚合所有的控制流。`End`节点也是代码分析的起点。如：

```c
void empty(void)
{
}
```

![](https://pp.ipd.kit.edu/firm/start_return.svg)

访问函数参数是通过对`Start`节点的结果映射到`T_args`再映射到对应参数的位置完成的：

```c
int add(int x, int y)
{
        return x + y;
}
```

![](https://pp.ipd.kit.edu/firm/params.svg)

对函数内局部变量的访问也比较类似，是通过对`Start`进行`Proj P P_frame_base`获取函数栈指针，然后再通过`Member P var`获取栈上`var`变量的具体地址。

`Start`节点也可以用来获取寄存器的值，可以用来访问`$zero`或者callee-saves寄存器等。

函数的调用节点`Call`需要提供函数指针、参数以及当前栈环境，其结果就是函数返回值。如：

```c
print_int(7);
print_int(4);
```

![](https://pp.ipd.kit.edu/firm/twocalls.svg)

## 主要难点

1. libFIRM本身是C语言组织的大型项目，而且其文档更多侧重于整体架构设计而并非在于内部代码，因此其内部代码理解起来有些困难。
2. Loongarch64本身的文档工作也没有非常充分，有些指令的翻译还需要参考https://godbolt.org/上面的GCC等其他编译器的结果。
3. libFIRM本身代码有Bug，导致调试的时候非常难受，因为难以断定不是自己的问题。多亏了[ASAN](https://github.com/google/sanitizers/wiki/AddressSanitizer)和[yComp](https://pp.ipd.kit.edu/firm/yComp.html)提供的可视化编译结果，我才能快速定位可能的代码问题。



## libFIRM后端编译总体流程

1. 发现、注册后端架构

   1. 首先需要在构建文件中声明新的后端
   2. 在`target.c` `ir_target_set_triple`中设定新架构的三元组描述
   3. 在`platform.c` `ir_platform_set`中设定新架构的编译器预定义宏
   4. 在`machine_triple.c` `ir_get_host_machine_triple`中也要正确解析新架构三元组
   5. 在`isas.h`中声明初始化函数`be_init_arch_loongarch64`和架构信息`loongarch64_isa_if`
   6. 在`bemodule.c` `be_init_modules`中调用初始化函数`be_init_arch_loongarch64()`

2. 定义后端架构

   1. 在`*arch_isa_if_t* const loongarch64_isa_if`中定义相关信息，包括
      1. 名字
      2. 指针大小，端序，最大对齐，移位运算上限
      3. 寄存器信息、数量与类别
      4. 初始化与清理函数
      5. `.generate` 核心代码生成函数
      6. `.lower_for_target` “底层化”函数
      7. `.get_op_estimated_cost` 节点开销函数，用于精细控制图生成算法
   2. 编写`loongarch64_spec.pl`文件，包括：
      1. 寄存器名字与类别
      2. Loongarch64相关的图节点，实际上基本上是能用到的指令的集合。其中值得注意的有：
         1. 为了应对LA64引入的较多后缀，我学习了Perl的基本语法并通过循环简化编写
         2. 根据godbolt的结果定义了各个零扩展、符号扩展的指令
         3. 使用了`maskeqz` `masknez` `or`指令组生成`Mux`节点
         4. 分支指令`Cond`的生成推迟到`emitter`阶段手动进行，因为`Cond`是双向跳转，而并非指令集中的单跳转。

3. 指令生成 `loongarch64_generate_code`

   1. 设定令`SP`寄存器跳过SSA约束检查，因为这个寄存器应当跳过寄存器分配并且本身不能有别名，而且还必须在内部多次赋值。

   2. `loongarch64_select_instructions` 指令选择。将FIRM IR的节点变换到LA64节点，也即LA64的指令组。并还执行`place_node`优化，移除不可达节点。

   3. `be_step_schedule` 图规划。为松散的图结构生成确定的线性指令生成顺序。

   4. 寄存器分配：

      1. 定义寄存器分配信息：

         ```c
         static const regalloc_if_t loongarch64_regalloc_if = {
             .spill_cost  = 1,
             .reload_cost = 1,
             .new_spill   = loongarch64_new_spill,
             .new_reload  = loongarch64_new_reload,
         };
         ```

         其中`loongarch64_new_spill` `loongarch64_new_reload`分别为标准的Spill/Reload函数，即把需要的寄存器存到栈空间或加载回来。内部实现基本上就是生成`st.d` `ld.d`指令，并通过`sched_add_after` `sched_add_before`插入对应的执行流中。需要Spill的变量会设置其`base`为栈指针，`entity`为空，来标志其为需要分配具体空间的变量。

      2. `be_step_regalloc(irg, &loongarch64_regalloc_if);` 根据给定的寄存器分配信息，使用FIRM默认的寄存器分配算法进行处理。

      3. `loongarch64_assign_spill_slots` 分配Spill槽

         1. `be_fec_env_t const fec_env = be_new_frame_entity_coalescer(irg);` 定义分配环境。这种编程模式在FIRM中经常使用，是在遍历图节点的时候保存一些全局信息使用。
         2. `irg_walk_graph(*irg*, NULL, loongarch64_collect_frame_entity_nodes, fec_env);` 使用我们的函数判断是否是需要分配空间的Spill。判断标准上面讲过了。使用`be_load_needs_frame_entity(env, node, 16, 4);`标记需要分配空间，并指定了大小和对齐信息。
         3. `be_assign_entities(fec_env, loongarch64_set_frame_entity, true);` 按照上面遍历得到的信息，再次遍历并分配对应的`entity`，可以理解为变量名。
         4. `be_free_frame_entity_coalescer(fec_env);` 释放分配环境。

   5. `be_sort_frame_entities` `be_layout_frame_type` 排序并排布栈空间分配

   6. `loongarch64_introduce_prologue_epilogue` 引入函数“入场曲”和“结束曲”

      1. 实际上是遍历整个图，为所有`Return`节点加入“结束曲”，并最后为`Start`节点加入“入场曲”
      2. 基本上是生成并插入`be_new_IncSP`节点调整`SP`寄存器的值

   7. `be_fix_stack_nodes(irg, &loongarch64_registers[REG_SP]);` 修复图结构让`SP`节点的相关操作也符合SSA形式

   8. `be_sim_stack_pointer(irg, 0, 4, &loongarch64_sp_sim);` 模拟栈指针变化，为所有局部变量（包括上面Spill出去的变量）分配具体的栈指针偏移量。`loongarch64_sp_sim`函数实际上就是判断该指令是否需要局部变量，并把变量名`entity`替换为栈偏移`offset`。

   9. `loongarch64_emit_function` 生成汇编代码

   10. `be_step_last(irg)` 结束，生成调试信息，回收资源



## 架构图变换 `loongarch64_transform.c`

### 普通二元指令

对于`Add` `Sub`等指令，我们定义助手函数：

```c
static ir_node *transform_common_binop(ir_node *node, ir_mode *provide_mode, bool is_commutative,
                                       new_binop_reg_func new_func_w, new_binop_reg_func new_func_d,
                                       new_binop_reg_func new_func_wu, new_binop_reg_func new_func_du,
                                       new_binop_imm_func new_func_wi, new_binop_imm_func new_func_di) {
    ir_mode  *mode  = provide_mode ? provide_mode : get_irn_mode(node);
    ir_node  *block = be_transform_nodes_block(node);
    ir_node  *op1   = get_binop_left(node);
    ir_node  *op2   = get_binop_right(node);
    dbg_info *dbgi  = get_irn_dbg_info(node);
    unsigned  bits  = get_mode_size_bits(mode);

    // 'mode-P' is pointer type, which is equal to 64-bit unsigned int.
    if (mode == mode_P) {
        mode = mode_Lu;
    }

    if (mode_is_int(mode)) {
        // Const folding
        if (new_func_wi && new_func_di) {
            // TODO: Both op1 and op2 are const. Generate a new const node.
            if (is_valid_si12(op2)) {
                op1 = be_transform_node(op1);
                if (bits == 32) {
                    return new_func_wi(dbgi, block, op1, NULL, get_Const_long(op2));
                } else if (bits == 64) {
                    return new_func_di(dbgi, block, op1, NULL, get_Const_long(op2));
                }
            }
            if (is_commutative && is_valid_si12(op1)) {
                op2 = be_transform_node(op2);
                if (bits == 32) {
                    return new_func_wi(dbgi, block, op2, NULL, get_Const_long(op1));
                } else if (bits == 64) {
                    return new_func_di(dbgi, block, op2, NULL, get_Const_long(op1));
                }
            }
        }

        ir_node *new_op1 = be_transform_node(op1);
        ir_node *new_op2 = be_transform_node(op2);
        if (!mode_is_signed(mode)) {
            if (bits == 32 && new_func_wu) {
                return new_func_wu(dbgi, block, new_op1, new_op2);
            } else if (bits == 64 && new_func_du) {
                return new_func_du(dbgi, block, new_op1, new_op2);
            }
        }
        if (bits == 32 && new_func_w) {
            return new_func_w(dbgi, block, new_op1, new_op2);
        } else if (bits == 64 && new_func_d) {
            return new_func_d(dbgi, block, new_op1, new_op2);
        }
    }

    // TODO: float-point support
    TODO(node);
}
```

1. 判断结果模式。对于指针`mode_P`，实际上是64位整数，变为`mode_Ls`。对于`Div` `Mod`，结果是`mode_T`，需要外部提供具体结果的类型。
2. 对于存在`<op>i`的指令，进行常量优化。对于第二个操作数是12位有符号整数的情况，生成指令。如果此操作是可交换的，并且第一个操作数是12位有符号整数，交换操作数并生成指令。
3. 其余情况，根据符号、位数生成对应的`w` `d` `wu` `du`。
4. 调用者对应的函数为NULL即声明不支持这类指令变种。

同时为函数参数定义以下助手宏：

```c
#define LA64_WD_INST(name) new_bd_loongarch64_##name##_w, new_bd_loongarch64_##name##_d
#define LA64_WDU_INST(name) \
    new_bd_loongarch64_##name##_w,  new_bd_loongarch64_##name##_d, \
    new_bd_loongarch64_##name##_wu, new_bd_loongarch64_##name##_du
#define LA64_WD_SAME_INST(name) new_bd_loongarch64_##name, new_bd_loongarch64_##name
```

这样我们就可以方便的定义很多节点变换：

```c
TRANS_FUNC(Add) { return transform_common_binop(node, NULL, true, LA64_WD_INST(add), NULL, NULL, LA64_WD_INST(addi)); }

TRANS_FUNC(Sub) { return transform_common_binop(node, NULL, false, LA64_WD_INST(sub), NULL, NULL, NULL, NULL); }

TRANS_FUNC(Mul) { return transform_common_binop(node, NULL, true, LA64_WD_INST(mul), NULL, NULL, NULL, NULL); }

TRANS_FUNC(Mulh) { return transform_common_binop(node, NULL, true, LA64_WDU_INST(mulh), NULL, NULL); }

TRANS_FUNC(Div) { return transform_common_binop(node, get_Div_resmode(node), false, LA64_WDU_INST(div), NULL, NULL); }

TRANS_FUNC(Mod) { return transform_common_binop(node, get_Mod_resmode(node), false, LA64_WDU_INST(mod), NULL, NULL); }

TRANS_FUNC(Shl) { return transform_common_binop(node, NULL, false, LA64_WD_INST(sll), NULL, NULL, LA64_WD_INST(slli)); }

TRANS_FUNC(Shr) { return transform_common_binop(node, NULL, false, LA64_WD_INST(srl), NULL, NULL, LA64_WD_INST(srli)); }

TRANS_FUNC(Shrs) {
    return transform_common_binop(node, NULL, false, LA64_WD_INST(sra), NULL, NULL, LA64_WD_INST(srai));
}

TRANS_FUNC(And) {
    return transform_common_binop(node, NULL, true, LA64_WD_SAME_INST(and), NULL, NULL, LA64_WD_SAME_INST(andi));
}

TRANS_FUNC(Or) {
    return transform_common_binop(node, NULL, true, LA64_WD_SAME_INST(or), NULL, NULL, LA64_WD_SAME_INST(ori));
}

TRANS_FUNC(Eor) {
    return transform_common_binop(node, NULL, true, LA64_WD_SAME_INST(xor), NULL, NULL, LA64_WD_SAME_INST(xori));
}
```

### 一些一元指令的处理

1. `Const`：如果为0，则取`$zero`。否则使用宏指令`li.w` `li.d`
2. `Minus`: `sub %D0, $zero, %S0`
3. `Not`: `nor %D0, $zero, %S0`

### 内存地址相关

Load/Store除了要根据模式选择对应的后缀，还需要`make_address`函数将内存地址转化为底层地址。

```c
static loongarch64_addr make_addr(ir_node *addr) {
    ir_entity *ent = 0;
    int64_t    val = 0;

    if (is_Add(addr)) {
        ir_node *const r = get_Add_right(addr);
        if (is_Const(r)) {
            long const v = get_Const_long(r);
            if (is_simm12(v)) {
                val  = v;
                addr = get_Add_left(addr);
            }
        }
    }

    if (is_Member(addr)) {
        ent  = get_Member_entity(addr);
        addr = get_Member_ptr(addr);
        assert(is_Proj(addr) && get_Proj_num(addr) == pn_Start_P_frame_base && is_Start(get_Proj_pred(addr)));
    }

    ir_node *const base = be_transform_node(addr);
    return (loongarch64_addr){base, ent, val};
}
```

1. 如果内存地址是`Add`产生的，并且第二个操作数为能放进Load/Store操作数中的常数，直接提取`base`和`val`
2. 如果地址是`frame_base`的`Member`，即局部变量，则保存栈指针作为`base`和变量名`ent`

FIRM中取全局变量/函数的地址的节点为`Address`，为它直接生成`la.address`宏指令。

`Member`节点的处理方式类似`make_addr`，最后返回一个`addi.d`指令指示局部变量的地址。

### 控制流相关

处理`Cmp`节点时，通过以下判断确定最后生成的`slt[u][i]`类型

1. `get_Cmp_relation(node)`获取比较的类型
2. 第二个操作数是否为12位有符号常量
3. 是否为有符号比较

如果有含有想等比较，说明最后需要结果取反，使用：`xori %D, %D, 1; andi %D, %D, 1`。

对于`Cond`节点，通过类似的比较判断生成的`blt[u]` `bge[u]`类型。同时，判断操作数是否有0，来生成`bltz` `bgez`。

对于`Mux`节点，通过以下指令序列生成：

```assembly
maskeqz %S1, %S1, %S0
masknez %D0, %S2, %S0
or      %D0, %D0, %S1
```

注意，这个指令序列更改了`%S1`，因此需要生成`be_Copy`节点先进行拷贝。

对于普通的`Jmp`，直接生成`b`指令。

对于`Switch`，在Lowering阶段已经全部转化为if-else序列，因此无需处理。

### 函数调用约定

首先我们定义以下结构体：

```c
typedef struct reg_or_slot_t {
    arch_register_t const *reg;
    unsigned               offset;
    ir_entity             *entity;
} reg_or_slot_t;

typedef struct calling_convention_t {
    size_t         n_params;
    reg_or_slot_t *parameters;
    reg_or_slot_t *results;
} calling_convention_t;
```

它保存了一个函数调用的参数个数，各个参数所在的寄存器或者栈空间的偏移量，各个返回值所在的寄存器。

在`setup_calling_convention`函数中，通过`ir_type const fun_type`描述的函数签名，确定`calling_convention_t`的值。

生成`Call`节点的过程如下：

1. 确定调用者，主要是在区分全局函数还是函数指针。
2. `setup_calling_convention`，并根据栈空间中的参数个数，生成`be_IncSP`节点增长栈空间。
3. 传入`SP`寄存器
4. 传入各个参数，如果是栈中参数，生成`st.d`储存
5. 生成`be_Sync`函数，用于同步所有的栈中参数的储存
6. 生成调用指令，`bl <func>`或`jirl %zero, %S0, 0`
7. 设置返回值需要的寄存器列表
8. 获取调用后的`SP`寄存器，还原存储参数的栈空间，记录栈空间变化。

生成`Return`节点的过程如下：

1. 设定`mem` `stack` `return_addr`参数
2. 保存各个返回值
3. 保存各个callee-saves寄存器
4. 生成`jr $ra`，并记录栈空间变化。

获取函数参数的方法，如上所述，是在`Proj -> Proj -> Start`中完成的：

1. 根据当前的调用约定，找到所需要的参数的信息
2. 如果为寄存器参数，直接通过`be_get_Start_proj`返回一个`Proj -> Start`节点获取寄存器的值
3. 如果为栈空间参数，生成`ld.d`加载参数

类似的，获取调用返回值的方法是`Proj -> Proj -> Call`中完成的，整体方法基本相同，只不过返回值只有从寄存器中获取。

在生成函数起点`Start`的时候，需要通过`be_start_out outs[N_LOONGARCH64_REGISTERS]`数组告知FIRM哪些寄存器是可通过`be_get_Start_proj`获取到的。

在主生成函数`loongarch64_transform_graph`中，通过`set_allocatable_regs`告知FIRM哪些寄存器是可分配的。设置调用约定后，还应为参数分配对应的`entity`，并使用`be_add_parameter_entity_stores`来处理后面需要参数内存地址的特殊情况（比如对寄存器参数`a`取地址`&a`）。

## 指令输出 `loongarch64_emitter.c`

1. 处理FIRM的特殊节点

   1. `be_Copy`: `ori %d, %s, 0`
   2. `be_IncSP`: `addi.d $sp, $sp, val`
   3. `be_Perm`: 交换，使用三个`xor`

2. 另外还有两个指令`loongarch64_b`和`loongarch64_b_cond`没有预设的emitter：

   1. ```c
      static void emit_jmp(ir_node const *const node, ir_node const *const target) {
          BE_EMIT_JMP(loongarch64, node, "b", target) { loongarch64_emitf(NULL, "nop"); }
      }
      ```

   2. `loongarch64_b`直接调用上面的函数

   3. `loongarch64_b_cond`首先判断是否为`z`指令来确定格式化序列。然后判断`true`分支是否直接fallthrough过去。如果fallthrough，只生成条件跳转到`false`分支。否则，生成条件跳转到`true`然后生成无条件跳转到`false`。

3. 对于立即数的处理有两种

   1. 对于`addi` `load`等立即数或偏移量，直接输出
   2. 对于`la.local` `bl`等描述全局地址的，通过`be_gas_emit_entity(ent);`输出

## “底层化”

FIRM提供了一套底层化函数帮助解构语言层面的高级特性。这里采用的有：

1. ```c
   static ir_settings_arch_dep_t const loongarch64_arch_dep = {
       .replace_muls         = true,
       .replace_divs         = true,
       .replace_mods         = true,
       .allow_mulhs          = true,
       .allow_mulhu          = true,
       .also_use_subs        = true,
       .maximum_shifts       = 4,
       .highest_shift_amount = 63,
       .evaluate             = NULL,
       .max_bits_for_mulh    = 64,
   };
   ```

2. 函数调用参数中的聚合类型使用指针传递

3. 底层化`be_CopyB`拷贝一块内存的函数。对于64字节以下的，使用Load/Store，对于更大的，使用`memcpy`

4. 底层化`switch`语句，目前直接全转化为if-else链

## 未来工作

1. 添加浮点数支持，要做的工作有：
   1. 在`spec`文件中添加浮点寄存器、浮点指令。
   2. `transform`时大多数二元指令只需更改助手函数即可。需要额外处理的是`Conv`节点需要负责浮点、整数的相互转化和函数调用约定相关的变化。
2. 添加变长数组支持，也即`Alloc`节点
   1. 在`SP`之外，还需要维护`TP`寄存器保存栈底指针，改用`TP`索引静态分配的栈上空间即可快速实现`Alloc`
3. `Builtin`相关
   1. 添加变长参数函数的定义支持。即支持`ir_bk_va_arg` `ir_bk_va_start`。这需要参考函数调用约定和ABI相关的文档。
   2. 添加`ir_bk_prefetch`支持，即生成`preld 0, %D0, 0`
   3. 添加`ir_bk_ffs` `ir_bk_clz` `ir_bk_ctz` `ir_bk_bswap`等支持。这些也都在Loongarch64有方便的指令级实现。
   4. 添加`ir_bk_trap`支持，即生成`break 0`
4. 添加内联汇编支持，即`ASM`节点
5. “底层化”相关
   1. 更高效的`CopyB`翻译。即对于小数组使用Load/Store，对于中数组可以尝试使用向量指令集扩展实现，对于大数组再使用`memcpy`。
   2. 更高效的`switch`翻译。即：
      1. 对于分支很少的，转化为if-else链
      2. 对于分支较多，但是分支条件有序且较为紧凑的，转化为跳转表
      3. 对于分支较多，但是分支条件有序但较为分散的，转化为二分搜索
      4. 对于其他情况，转化为if-else链
      5. 某些特殊情况，如对`flag`类数据做`switch`，即每个分支只判断一位数据，则可以用位运算来生成。
6. 探索更多指令集相关的优化和FIRM IR相关的优化
   1. 目前观察到寄存器分配、callee-save恢复相关的代码会引入过多的`be_Copy`，这需要进一步调查。
   2. 但是似乎FIRM内部的优化代码有些问题，即使使用amd64后端，在开启`-O2`对情况下自己可能会崩溃。所以这部分也被迫放缓。

