# the cpu architecture (ia32, ia64, mips, sparc, ppc, ...)
$arch = "loongarch64";

# Modes
$mode_gp = "mode_Iu"; # mode used by general purpose registers
$mode_fp = "mode_F";  # mode used by floatingpoint registers

# The node description is done as a perl hash initializer with the
# following structure:
#
# %nodes = (
#
# <op-name> => {
#   state     => "floats|pinned|mem_pinned|exc_pinned", # optional, default floats
#   comment   => "any comment for constructor",  # optional
#   in_reqs   => [ "reg_class|register" ] | "...",
#   out_reqs  => [ "reg_class|register|in_rX" ] | "...",
#   ins       => { "in1", "in2" },  # optional, creates n_op_in1, ... consts
#   outs      => { "out1", "out2" },# optional, creates pn_op_out1, ... consts
#   mode      => "first" | "<mode>" # optional, determines the mode, auto-detected by default
#   emit      => "emit code with templates",   # optional for virtual nodes
#   attr      => "additional attribute arguments for constructor", # optional
#   init      => "emit attribute initialization template",         # optional
#   hash_func => "name of the hash function for this operation",   # optional, get the default hash function else
#   attr_type => "name of the attribute struct",                   # optional
# },
#
# ... # (all nodes you need to describe)
#
# );

%reg_classes = (
	gp => {
		mode => $mode_gp,
		registers => [
			{ name => "zero", encoding =>  0 },
			{ name => "ra",   encoding =>  1 },
			{ name => "tp",   encoding =>  2 },
			{ name => "sp",   encoding =>  3 },
			{ name => "a0",   encoding =>  4 },
			{ name => "a1",   encoding =>  5 },
			{ name => "a2",   encoding =>  6 },
			{ name => "a3",   encoding =>  7 },
			{ name => "a4",   encoding =>  8 },
			{ name => "a5",   encoding =>  9 },
			{ name => "a6",   encoding => 10 },
			{ name => "a7",   encoding => 11 },
			{ name => "t0",   encoding => 12 },
			{ name => "t1",   encoding => 13 },
			{ name => "t2",   encoding => 14 },
			{ name => "t3",   encoding => 15 },
			{ name => "t4",   encoding => 16 },
			{ name => "t5",   encoding => 17 },
			{ name => "t6",   encoding => 18 },
			{ name => "t7",   encoding => 19 },
			{ name => "t8",   encoding => 20 },
			{ name => "r21",  encoding => 21 },
			{ name => "fp",   encoding => 22 },
			{ name => "s0",   encoding => 23 },
			{ name => "s1",   encoding => 24 },
			{ name => "s2",   encoding => 25 },
			{ name => "s3",   encoding => 26 },
			{ name => "s4",   encoding => 27 },
			{ name => "s5",   encoding => 28 },
			{ name => "s6",   encoding => 29 },
			{ name => "s7",   encoding => 30 },
			{ name => "s8",   encoding => 31 },
		]
	},
);

%init_attr = (
	loongarch64_attr_t => "",
	loongarch64_immediate_attr_t => "attr->ent = ent;\n\tattr->val = val;",
);

my $int_d_rr_op = {
	irn_flags => [ "rematerializable" ],
	in_reqs   => [ "gp", "gp" ],
	out_reqs  => [ "gp" ],
	emit      => "{name}.D\t%D0\t%S0\t%S1",
};

my $int_rr_op = {
	irn_flags => [ "rematerializable" ],
	in_reqs   => [ "gp", "gp" ],
	out_reqs  => [ "gp" ],
	emit      => "{name}\t%D0\t%S0\t%S1",
};

my $int_d_rc_op = {
	irn_flags => [ "rematerializable" ],
	in_reqs   => [ "gp", "gp" ],
	out_reqs  => [ "gp" ],
	attr_type => "loongarch64_immediate_attr_t",
	attr      => "ir_entity *const ent, ir_tarval *const val",
	emit      => "{name}.D\t%D0\t%S0\t%I",
};

my $int_rc_op = {
	irn_flags => [ "rematerializable" ],
	in_reqs   => [ "gp", "gp" ],
	out_reqs  => [ "gp" ],
	init      => "attr->ent = ent;\n\tattr->val = val;",
	attr      => "ir_entity *const ent, ir_tarval *const val",
	emit      => "{name}\t%D0\t%S0\t%I",
};

my $constop = {
	op_flags   => [ "constlike" ],
	irn_flags  => [ "rematerializable" ],
	out_reqs   => [ "gp" ],
};

my $fbinop = {
	in_reqs   => [ "cls-fp", "cls-fp" ],
	out_reqs  => [ "cls-fp" ],
	emit      => "{name}\t%D0\t%S0\t%S1",
};

my $unop = {
	irn_flags => [ "rematerializable" ],
	in_reqs   => [ "gp" ],
	out_reqs  => [ "gp" ],
	emit      => "{name}\t%D0\t%S0",
};

%nodes = (

Const => {
	template => $constop,
	attr     => "ir_entity *entity, ir_tarval *value",
	# init     => "set_template_value(res, entity, value);",
	emit     => "%D0 = const %I",
},

# Control Flow

Jmp => {
	state     => "pinned",
	op_flags  => [ "cfopcode" ],
	irn_flags => [ "simple_jump", "fallthrough" ],
	out_reqs  => [ "exec" ],
},

Return => {
	state    => "pinned",
	op_flags => [ "cfopcode" ],
	in_reqs  => "...",
	out_reqs => [ "exec" ],
	ins      => [ "mem", "stack", "first_result" ],
	outs     => [ "X" ],
},

# Load / Store

Load => {
	op_flags  => [ "uses_memory" ],
	irn_flags => [ "rematerializable" ],
	state     => "exc_pinned",
	in_reqs   => [ "mem", "gp" ],
	out_reqs  => [ "gp", "mem" ],
	ins       => [ "mem", "ptr" ],
	outs      => [ "res", "M" ],
	emit      => "%D0 = load (%S1)",
},

Store => {
	op_flags  => [ "uses_memory" ],
	irn_flags => [ "rematerializable" ],
	state     => "exc_pinned",
	in_reqs   => [ "mem", "gp", "gp" ],
	out_reqs  => [ "mem" ],
	ins       => [ "mem", "ptr", "val" ],
	outs      => [ "M" ],
	emit      => "(%S1) = store %S2",
},

);

# Generate instructions with post-fix

my @rr_op_wd = (
	"add", "sub", "mul", "sll", "srl", "sra", "rotr",
);

my @rc_op_wd = (
	"addi", "slli", "srli", "srai", "rotri",
);

my @rr_op_wdu = (
	"mulh", "div", "mod",
);

my @rr_op = (
	"and", "or", "nor", "xor", "andn", "orn",
);

my @rc_op = (
	"andi", "ori", "xori",
);

sub add_rr_op_with_postfix {
	my $op = shift;
	my $postfix = shift;
	$nodes{"${op}_${postfix}"} = {
		irn_flags => [ "rematerializable" ],
		in_reqs   => [ "gp", "gp" ],
		out_reqs  => [ "gp" ],
		emit      => "${op}.${postfix}\t%D0\t%S0\t%S1",
	};
}

for my $op (@rr_op_wd) {
	add_rr_op_with_postfix($op, "w");
	add_rr_op_with_postfix($op, "d");
}

for my $op (@rr_op_wdu) {
	add_rr_op_with_postfix($op, "w");
	add_rr_op_with_postfix($op, "wu");
	add_rr_op_with_postfix($op, "d");
	add_rr_op_with_postfix($op, "du");
}

sub add_rc_op_with_postfix {
	my $op = shift;
	my $postfix = shift;
	$nodes{"${op}_${postfix}"} = {
		irn_flags => [ "rematerializable" ],
		in_reqs   => [ "gp" ],
		out_reqs  => [ "gp" ],
		attr_type => "loongarch64_immediate_attr_t",
		attr      => "ir_entity *const ent, ir_tarval *const val",
		emit      => "${op}.${postfix}\t%D0,\t%S0,\t%I",
	};
}

for my $op (@rc_op_wd) {
	add_rc_op_with_postfix($op, "w");
	add_rc_op_with_postfix($op, "d");
}

add_rc_op_with_postfix("addu16i", "d");

sub add_rr_op {
	my $op = shift;
	$nodes{"${op}"} = {
		irn_flags => [ "rematerializable" ],
		in_reqs   => [ "gp", "gp" ],
		out_reqs  => [ "gp" ],
		emit      => "${op}\t%D0,\t%S0,\t%S1",
	};
}

add_rr_op for @rr_op;

sub add_rc_op {
	my $op = shift;
	$nodes{"${op}"} = {
		irn_flags => [ "rematerializable" ],
		in_reqs   => [ "gp" ],
		out_reqs  => [ "gp" ],
		attr_type => "loongarch64_immediate_attr_t",
		attr      => "ir_entity *const ent, ir_tarval *const val",
		emit      => "${op}\t%D0,\t%S0,\t%I",
	};
}

add_rc_op for @rc_op;

# Load/Store
for my $postfix ("b", "h", "w", "d", "wu", "hu", "du") {
	$nodes{"ld_${postfix}"} = {
		state     => "exc_pinned",
		in_reqs   => [ "mem", "gp" ],
		out_reqs  => [ "mem", "gp" ],
		ins       => [ "mem", "base" ],
		outs      => [ "M", "res" ],
		attr_type => "loongarch64_immediate_attr_t",
		attr      => "ir_entity *const ent, ir_tarval *const val",
		emit      => "ld_${postfix}\t%D1,\t%A",
	};
}
for my $postfix ("b", "h", "w", "d") {
	$nodes{"st_${postfix}"} = {
		state     => "exc_pinned",
		in_reqs   => [ "mem", "gp", "gp" ],
		out_reqs  => [ "mem" ],
		ins       => [ "mem", "base", "value" ],
		outs      => [ "M" ],
		attr_type => "loongarch64_immediate_attr_t",
		attr      => "ir_entity *const ent, ir_tarval *const val",
		emit      => "st_${postfix}\t%S2,\t%A",
	};
}

%nodes;