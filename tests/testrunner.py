import sys
import errno
import os
import re
import subprocess
import signal
import multiprocessing
from collections import namedtuple
from abc import ABCMeta, abstractmethod
from check import map_by_build_id
from build_id import get_build_id
import traceback


class Test:
	def __init__(self, path, test_type):
		self.path = path
		self.test_type = test_type
		self.stdout = None
		self.stderr = None
		self.returncode = None
		self.__state__ = "init"
		self.p = None
		self.__set_test_env__(os.path.dirname(path))

	def __set_test_env__(self, library_path):
		ld_library_path = ""
		try:
			ld_library_path = os.environ['LD_LIBRARY_PATH']
		except:
			pass
		os.environ['LD_LIBRARY_PATH'] = ld_library_path + ":" + library_path

	def start(self):
		cmd = "%s -t %d -n %d" % (self.path, self.test_type, 2)
		try:
			if self.__state__ != "init":
				print "Test is not new. State: %s" % self.__state__
				raise

			print "Execute: %s" % cmd
			self.p = subprocess.Popen(cmd.split(), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
			if self.p.poll():
				stdout, stderr = self.p.communicate()
				print stdout
				print stderr
				return None
			self.__state__ = "started"
		except NameError as e:
			print "Unexpected NameError: %s" % e
		except OSError as e:
			print "Unexpected start OSError: '%s', %s" % (' '.join(args), e.strerror)
		except:
			print "Unexpected start error:", sys.exc_info()[0]
		return self.p.pid

	def wait(self):
		try:
			if self.__state__ != "started":
				print "Process is not started. State: %s" % self.__state__
				raise

			self.returncode = self.p.wait()
			self.stdout, self.stderr = self.p.communicate()
			self.__state__ = "stopped"

		except OSError as e:
			print "Unexpected OSError: %s, %s" % (e.filename, e.strerror)
			return 1
		except:
			print "Failed to wait process %d\n" % self.p.pid
			print "Unexpected wait error:", sys.exc_info()[0]
			return self.returncode
		return 0

	def signal(self, signo):
		try:
			if self.__state__ != "started":
				print "Process is not started. State: %s" % self.__state__
				return

			os.kill(self.p.pid, signo)
		except OSError as e:
			print "Unexpected OSError: %s, %s" % (e.filename, e.strerror)
			return 1
		except:
			print "Failed to stop process %d\n" % self.p.pid
			print "Unexpected stop error:", sys.exc_info()[0]
			return 1
		return self.wait()

	def stop(self):
		return self.signal(signal.SIGINT)

	def kill(self):
		return self.signal(signal.SIGKILL)

	def is_running(self):
		try:
			os.kill(self.p.pid, 0)
		except OSError:
			return False
		return True

	def check_result(self):
		if self.returncode:
			print "************ Error ************"
			print "Test %s (pid %d): exited with %d" % (self.path, self.p.pid, self.returncode)
			print "stdout:\n%s" % self.stdout
			print "stderr:\n%s" % self.stderr
			return 1

		print "************ Pass *************"
		return 0

	def get_map_path(self, bid):
		tries = 50
		while tries:
			try:
				return map_by_build_id(self.p.pid, bid)
			except IOError as e:
				import time
				time.sleep(1 / 1000000)
				tries -= 1
				continue
		raise


class BinPatch:
	__metaclass__ = ABCMeta

	def __init__(self, source, target, no_plugin):
		self.source = source
		self.target = target
		self.no_plugin = no_plugin

		self.generator = os.environ.get('NSB_GENERATOR')
		if not self.generator:
			print "NSB_GENERATOR environment variable must be set"
			raise

		self.patcher = os.environ.get('NSB_PATCHER')
		if not self.patcher:
			print "NSB_PATCHER environment variable must be set"
			raise

	def __run_cmd__(self, cmd):
		print "Execute: %s" % cmd
		return subprocess.Popen(cmd.split(), stdout=subprocess.PIPE, stderr=subprocess.PIPE)

	def exec_cmd(self, cmd):
		try:
			p = self.__run_cmd__(cmd)
			stdout, stderr = p.communicate()
		except OSError as e:
			print "Unexpected OSError: %s, %s" % (e.filename, e.strerror)
			raise
		except:
			print "Unexpected error:", sys.exc_info()[0]
			raise
		print stdout
		print stderr
		return p.returncode

	@abstractmethod
	def generate_patch(self): pass

	def apply_patch(self, test):
		cmd = "%s patch -v 4 -f %s -p %d" % (self.patcher, self.target, test.p.pid)
		if self.no_plugin:
			cmd += " --no-plugin"
		return self.exec_cmd(cmd)

	def check_patch(self, test):
		return self.exec_cmd("%s check -v 4 -f %s -p %d" % (self.patcher, self.target, test.p.pid))

	def list_patches(self, test):
		return self.exec_cmd("%s list -p %d" % (self.patcher, test.p.pid))

	def revert_patch(self, test):
		return self.exec_cmd("%s revert -v 4 -f %s -p %d" % (self.patcher, self.target, test.p.pid))


class ManualBinPatch(BinPatch):
	def generate_patch(self):
		return self.exec_cmd("python %s generate %s %s --mode manual" %
				(self.generator, self.source, self.target))


class AutoBinPatch(BinPatch):
	def __init__(self, source, target, no_plugin, target_obj):
		BinPatch.__init__(self, source, target, no_plugin)
		self.target_obj = target_obj

	def generate_patch(self):
		return self.exec_cmd("python %s generate %s %s %s --mode auto" %
				(self.generator, self.source,
					self.target, self.target_obj))


class LivePatchTest:
	__metaclass__ = ABCMeta

	def __init__(self, source, target, no_plugin, target_obj, test_type, patch_mode):
		try:
			self.tests_dir = os.environ['NSB_TESTS'] + '/'
		except:
			self.tests_dir = os.getcwd() + '/tests/'

		self.test_bin = self.test_binary(source)
		self.src_elf = self.source_elf(source)
		self.tgt_elf = self.target_elf(target)
		self.target_obj = self.target_elf(target_obj)
		self.test_type = test_type
		self.patch_mode = patch_mode
		self.no_plugin = no_plugin

	def __do_apply_patch_test__(self, patch, test):
		res = patch.check_patch(test)
		if res == 0:
			print "Patch is considered as applied"
			raise
		if res != errno.ENOENT:
			print "Failed to check whether patch is applied"
			raise

		if patch.apply_patch(test) != 0:
			print "Failed to apply binary patch\n"
			raise

		if patch.apply_patch(test) == 0:
			print "Binary patch successfully applied twise\n"
			raise

		if patch.list_patches(test) != 0:
			print "Failed to list applied patches\n"
			raise

	def __do_revert_patch_test__(self, patch, test):
		res = patch.check_patch(test)
		if res == errno.ENOENT:
			print "Patch is considered as unapplied"
			raise
		if res > 1:
			print "Failed to check whether patch is applied"
			raise

		if patch.revert_patch(test) != 0:
			print "Failed to revert applied patch\n"
			raise

		if patch.revert_patch(test) == 0:
			print "Binary patch successfully reverted twise\n"
			raise


	def __do_test__(self, test):
		try:
			bid = self.get_elf_bid(self.src_elf)
		except:
			traceback.print_exc()
			print "failed to get \"%s\" Build-ID" % self.src_elf
			raise

		print "ELF \"%s\" Build-ID: %s" % (self.src_elf, bid)

		try:
			source = test.get_map_path(bid)
		except:
			traceback.print_exc()
			print "failed to find map by \" Build-ID" % bid
			raise

		print "Test map by Build-ID: %s" % source

		if self.patch_mode == "manual":
			patch = ManualBinPatch(source, self.tgt_elf, self.no_plugin)
		elif self.patch_mode == "auto":
			patch = AutoBinPatch(source, self.tgt_elf, self.no_plugin, self.target_obj)
		else:
			print "Unknown patch mode: %s" % self.patch_mode
			raise

		if patch.generate_patch() != 0:
			print "Failed to generate binary patch\n"
			raise

		self.__do_apply_patch_test__(patch, test)

		self.__do_revert_patch_test__(patch, test)

		self.__do_apply_patch_test__(patch, test)

		return

	def run(self):
		print "Starting test %s" % self.test_bin
		test = Test(self.test_bin, self.test_type)

		if test.start() is None:
			print "Failed to start process %s\n" % test.path
			return 1

		try:
			self.__do_test__(test)
		except:
			test.kill()
			return 1

		if test.stop() != 0:
			return 1

		return test.check_result()

	def get_elf_bid(self, path):
		return get_build_id(path)

	@abstractmethod
	def test_binary(self, path): pass

	@abstractmethod
	def source_elf(self, path): pass

	def target_elf(self, path):
		return self.tests_dir + path


class LibraryLivePatchTest(LivePatchTest):
	def test_binary(self, path):
		return self.tests_dir + ".libs/" + path

	def source_elf(self, path):
		return self.tests_dir + ".libs/libtest.so"


class ExecutableLivePatchTest(LivePatchTest):
	def test_binary(self, path):
		return self.tests_dir + path

	def source_elf(self, path):
		return self.tests_dir + path
