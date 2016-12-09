from collections import namedtuple

from elftools.elf.elffile import ELFFile
from elftools.elf.elffile import SymbolTableSection
from elftools.elf.descriptions import describe_p_flags, describe_reloc_type
from elftools.elf.constants import P_FLAGS

ElfHeader = namedtuple("ElfHeader", "type machine")
ElfSym = namedtuple("ElfSym", "num value size type bind vis ndx name")
ElfSection = namedtuple("ElfSection", "offset addr size")
ElfSegment = namedtuple("ElfSegment", "type offset vaddr paddr mem_sz flags align file_sz")
ElfRelaPlt = namedtuple("ElfRelaPlt", "offset info_type addend")


class ElfFile:
	def __init__(self, stream):
		self.stream = stream
		self.elf = ELFFile(self.stream)

	def get_header(self):
		return ElfHeader(self.elf['e_type'],
				 self.elf['e_machine'])
	def __section_symbols__(self, section_name):
		symbols = {}

		section = self.elf.get_section_by_name(section_name)
		if section is None:
			return None

		for num in range(0, section.num_symbols()):
			s = section.get_symbol(num)
			symbols[num] = ElfSym(num, s['st_value'],
					s['st_size'], s['st_info'].type,
					s['st_info'].bind,
					s['st_other'].visibility,
					s['st_shndx'], s.name)

		return symbols

	def get_symbols(self):
		return self.__section_symbols__('.symtab')

	def get_dyn_symbols(self):
		return self.__section_symbols__('.dynsym')

	def get_sections(self):
		sections = {}
		for i in range(self.elf.num_sections()):
			s = self.elf.get_section(i)
			sections[s.name] = ElfSection(s['sh_offset'],
						      s['sh_addr'], s['sh_size'])
		return sections

	def get_segments(self):
		segments = []
		for s in self.elf.iter_segments():
			segment = ElfSegment(s['p_type'], s['p_offset'],
					s['p_vaddr'], s['p_paddr'],
					s['p_memsz'], s['p_flags'],
					s['p_align'], s['p_filesz'])
			segments.append(segment)
		return segments

	def get_rela_plt(self, symbols):
		rela_plt = {}
		section = self.elf.get_section_by_name('.rela.plt')
		for rel in section.iter_relocations():
			s = symbols[rel['r_info_sym']]
			rela_plt[s.name] = ElfRelaPlt(rel['r_offset'],
						describe_reloc_type(rel['r_info_type'], self.elf),
						s.value)
			print rela_plt
		return rela_plt

	def get_rela_dyn(self, symbols):
		rela_dyn = {}
		section = self.elf.get_section_by_name('.rela.dyn')
		for rel in section.iter_relocations():
			s = symbols[rel['r_info_sym']]
			rela_dyn[s.name] = ElfRelaPlt(rel['r_offset'],
						describe_reloc_type(rel['r_info_type'], self.elf),
						s.value)
		return rela_dyn