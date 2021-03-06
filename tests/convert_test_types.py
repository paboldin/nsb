import argparse
import os
import re

def convert_enum(header):
	src = os.open(header, os.O_RDONLY)
	content = os.read(src, 4096)
	start = content.find("enum")
	end = content.find("test_type_t")
	enum = content[start:end]

	test_types = re.findall('TEST_TYPE_[^,]*', enum)

	code =	"#!/usr/bin/env python2\n"
	code += "NSB_TEST_TYPES = dict(\n"
	nr = 0
	for t in test_types:
		if t.find("=") != -1:
			print "Enumerated enums are not supported"
			exit(1)
		code += "\t%s=%d,\n" % (t, nr)
		nr += 1
	code += ")\n"

	header_dirname = os.path.dirname(header)
	header_basename = os.path.basename(header)

	pyfile = header_dirname + "/nsb_" + header_basename[:header_basename.find('.')] + ".py"
	dst = os.open(pyfile, os.O_WRONLY | os.O_CREAT | os.O_TRUNC)
	os.write(dst, code)

parser = argparse.ArgumentParser()
parser.add_argument("header", help="C-header file")
args = parser.parse_args()

convert_enum(args.header)
