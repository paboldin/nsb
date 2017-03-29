from __future__ import print_function

import array
import bisect
import functools
from weakref import WeakKeyDictionary
from elftools.dwarf import enums, dwarf_expr
from elftools.dwarf.die import DIE

from consts import *

set_const_str(enums.ENUM_DW_TAG)
set_const_str(enums.ENUM_DW_AT)
set_const_str(enums.ENUM_DW_FORM)
set_const_str(dwarf_expr.DW_OP_name2opcode)

def format_di_key(di_key):
	suffix_map = {
		STR.DW_TAG_compile_unit:	'::',
		STR.DW_TAG_subprogram:		'()::',
		STR.DW_TAG_variable:		'',
	}
	get_suffix = lambda tag: suffix_map.get(tag, '??')
	return ''.join(name + get_suffix(tag) for name, tag in di_key)

class ExprVisitor(dwarf_expr.GenericExprVisitor):
	def __init__(self, structs):
		super(ExprVisitor, self).__init__(structs)
		self.__value = None

	def _after_visit(self, opcode, opcode_name, args):
		if opcode_name != STR.DW_OP_addr:
			raise Exception("Unsupported opcode {0}".format(opcode_name))

		self.__value = args[0]

	def get_addr(self, expr):
		self.process_expr(expr)
		return self.__value

def get_die_name(die):
	attr = die.attributes[STR.DW_AT_name]
	assert attr.form in [STR.DW_FORM_string, STR.DW_FORM_strp], attr.form
	return attr.value

def get_die_key(die):
	if die.tag not in [STR.DW_TAG_subprogram, STR.DW_TAG_variable]:
		return

	skip_attrs = [
		STR.DW_AT_abstract_origin,
		STR.DW_AT_declaration,
		STR.DW_AT_artificial,
	]
	if set(die.attributes).intersection(skip_attrs):
		return

	result = []
	while die:
		if die.tag == STR.DW_TAG_lexical_block:
			return
		sym_name = get_die_name(die)
		result.append((sym_name, die.tag))
		die = die.get_parent()

	result.reverse()
	return tuple(result)

def get_die_addr(die):
	structs = die.cu.structs

	if die.tag == STR.DW_TAG_subprogram:
		if STR.DW_AT_entry_pc in die.attributes:
			raise Exception("DW_AT_entry_pc is not supported")

		attr = die.attributes[STR.DW_AT_low_pc]
		assert attr.form == STR.DW_FORM_addr, attr.form
		return attr.value

	elif die.tag == STR.DW_TAG_variable:
		attr = die.attributes[STR.DW_AT_location]
		assert attr.form == STR.DW_FORM_exprloc, attr.form

		expr_visitor = ExprVisitor(structs)
		return expr_visitor.get_addr(attr.value)

	else:
		assert 0

def memoize(dict_class):
	def fix_dict_class(f):
		cache = dict_class()

		@functools.wraps(f)
		def wrapper(arg):
			res = cache.get(arg)
			if res is not None:
				return res
			res = cache[arg] = f(arg)
			return res

		return wrapper

	return fix_dict_class

@memoize(WeakKeyDictionary)
def _read_CU(cu):
	die_pos        = array.array('l', [-1])
	die_parent_pos = array.array('l', [-1])

	# See CompileUnit._unflatten_tree()
	cu_boundary = cu.cu_offset + cu['unit_length'] + cu.structs.initial_length_field_size()
	die_offset = cu.cu_die_offset
	parent_stack = [-1]
	while die_offset < cu_boundary:
		die = DIE(
			cu=cu,
			stream=cu.dwarfinfo.debug_info_sec.stream,
			offset=die_offset)

		if not die.is_null():
			die_pos.append(die_offset)
			die_parent_pos.append(parent_stack[-1])
			if die.has_children:
				parent_stack.append(die_offset)
		elif parent_stack:
			parent_stack.pop()

		die_offset += die.size

	return die_pos, die_parent_pos

class DebugInfo(object):
	def __init__(self, elf):
		self.elf = elf
		self._cu_pos  = cu_pos  = []

		if not self.elf.has_dwarf_info():
			raise Exception("No debuginfo in ELF")
		dwi = self.elf.get_dwarf_info()

		for cu in dwi.iter_CUs():
			cu_pos.append((-cu.cu_offset, cu))

		cu_pos.append((1, None, None))
		cu_pos.sort()

	def lookup_die(self, pos):
		assert pos >= 0
		# Consider sorted array A  having no duplicate elements
		# [..., X, Y, ...], where X < Y, and some element P
		# If X < P < Y then bisect_left(P) == bisect_right(P) == index(Y)
		# as described at https://docs.python.org/2/library/bisect.html
		# IOW, bisection selects right end of the range. Finally, when
		# P is same as Y, these functions return different results:
		# bisect_left(P)  == index(Y)
		# bisect_right(P) == index(Y) + 1
		# So we use A[bisect_right(pos) - 1] to lookup DIEs.
		# When looking up CUs, situation is a bit different, since we store
		# 3-tuples in the array. To make comparisons possible, we should use 1-tuple as a key.
		# When position to look up matches CU offset, key tuple will be less than element tuple.
		# So subtracting one will give wrong result. To overcome this, we use negated offsets.
		# In such case, we want to select the right end, so to lookup CUs we use
		# A[bisect_right(key)]
		# bisect_right() is the same as bisect()
		cu_key =(-pos,)
		cu_idx = bisect.bisect(self._cu_pos, cu_key)
		_, cu = self._cu_pos[cu_idx]
		if not cu:
			return
		die_pos, die_parent_pos = _read_CU(cu)

		die_idx = bisect.bisect(die_pos, pos)
		assert die_idx > 0
		die_idx -= 1
		die_offset = die_pos[die_idx]
		if die_offset < 0:
			return

		# See CompileUnit._parse_DIEs()
		die = DIE(
			cu=cu,
			stream=cu.dwarfinfo.debug_info_sec.stream,
			offset=die_offset)
		within_die = die.offset <= pos < die.offset + die.size
		return (die, die_parent_pos[die_idx]) if within_die else (None, None)

