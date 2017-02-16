#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <dlfcn.h>

#include "include/patch.h"
#include "include/log.h"
#include "include/xmalloc.h"
#include "include/vma.h"
#include "include/elf.h"

#include "include/process.h"
#include "include/x86_64.h"
#include "include/backtrace.h"
#include "include/rtld.h"

struct process_ctx_s process_context;

static const struct patch_ops_s *set_patch_ops(const char *how, const char *type);

struct patch_ops_s {
	char *name;
	int (*apply_patch)(struct process_ctx_s *ctx);
	int (*set_jumps)(struct process_ctx_s *ctx);
	int (*check_backtrace)(const struct process_ctx_s *ctx,
			       const struct backtrace_s *bt);
	int (*cleanup_target)(struct process_ctx_s *ctx);
};

static const struct funcpatch_s *search_func_by_name(const struct binpatch_s *bp, const char *name)
{
	const struct funcpatch_s *funcpatch;

	list_for_each_entry(funcpatch, &bp->functions, list) {
		if (!strcmp(funcpatch->fp->name, name))
			return funcpatch;
	}
	return NULL;
}

static int apply_objinfo(struct process_ctx_s *ctx, unsigned long start, ObjInfo *oi)
{
	unsigned char code[X86_MAX_SIZE];
	ssize_t size;
	int err, i;
	unsigned long where = start + oi->offset;

	pr_debug("\t\tinfo: name     : %s\n", oi->name);
	pr_debug("\t\tinfo: op_size  : %#x\n", oi->op_size);
	pr_debug("\t\tinfo: addr_size: %#x\n", oi->addr_size);
	pr_debug("\t\tinfo: offset   : %#x\n", oi->offset);
	pr_debug("\t\tinfo: ref_addr : %#x\n", oi->ref_addr);

	if (oi->ref_addr == 0) {
		const struct funcpatch_s *funcpatch;

		/* This means, that function is a new one */
		funcpatch = search_func_by_name(&ctx->binpatch, oi->name);
		if (!funcpatch) {
			pr_err("\t\tfailed to find function by name %s\n", oi->name);
			return -EINVAL;
		}
		pr_debug("\t\tfunction address : %#lx\n", funcpatch->addr);
		oi->ref_addr = funcpatch->addr;
	}

	err = process_read_data(ctx->pid, where, code, round_up(sizeof(code), 8));
	if (err < 0) {
		pr_err("failed to read process address %ld: %d\n", where, err);
		return err;
	}

	pr_debug("\t\tinfo: old code :");
	for (i = 0; i < X86_MAX_SIZE; i++)
		pr_msg(" %02x", code[i]);
	pr_debug("\n");

	size = x86_modify_instruction(code, oi->op_size, oi->addr_size,
				      where, oi->ref_addr);
	if (size < 0)
		return size;

	pr_debug("\t\tinfo: new code :");
	for (i = 0; i < X86_MAX_SIZE; i++)
		pr_msg(" %02x", code[i]);
	pr_debug("\n");

	err = process_write_data(ctx->pid, where, code, round_up(sizeof(code), 8));
	if (err < 0) {
		pr_err("failed to write process address %ld: %d\n", where, err);
		return err;
	}
	return 0;
}

static int apply_funcpatch(struct process_ctx_s *ctx, unsigned long addr, FuncPatch *fp)
{
	int i, err = 0;
	unsigned char jump[X86_MAX_SIZE];
	ssize_t size;

	pr_debug("\tpatch: name : %s\n", fp->name);
	pr_debug("\tpatch: addr : %#lx\n", fp->addr);
	pr_debug("\tpatch: size : %d\n", fp->size);
	pr_debug("\tpatch: new  : %d\n", fp->new_);
	pr_debug("\tpatch: code :");
	for (i = 0; i < fp->size; i++)
		pr_msg(" %02x", fp->code.data[i]);
	pr_debug("\n");
	pr_debug("\tplace address  : %#lx\n", addr);

	err = process_write_data(ctx->pid, addr, fp->code.data,
				 round_up(fp->size, 8));
	if (err < 0) {
		pr_err("failed to patch: %d\n", err);
		return err;
	}

	for (i = 0; i < fp->n_objs; i++) {
		pr_debug("\tObject info %d:\n", i);
		err = apply_objinfo(ctx, addr, fp->objs[i]);
		if (err)
			return err;
	}

	if (!fp->new_) {
		pr_debug("\tredirect to %#lx (overwrite %#lx)\n", addr, fp->addr);
		size = x86_jmpq_instruction(jump, fp->addr, addr);
		if (size < 0)
			return size;

		err = process_write_data(ctx->pid, fp->addr, jump, round_up(size, 8));
		if (err < 0)
			pr_err("failed to patch: %d\n", err);
	}
	return err;
}

static int apply_exec_binpatch(struct process_ctx_s *ctx)
{
	int i, err = 0;
	struct binpatch_s *binpatch = &ctx->binpatch;
	BinPatch *bp = binpatch->bp;
	struct funcpatch_s *funcpatch;

	for (i = 0; i < bp->n_patches; i++) {
		FuncPatch *fp = bp->patches[i];
		unsigned long addr;

		addr = process_get_place(ctx, fp->addr, fp->size);
		if (addr < 0)
			return addr;

		funcpatch = xmalloc(sizeof(*funcpatch));
		if (!funcpatch)
			return -ENOMEM;

		funcpatch->addr = addr;
		funcpatch->fp = fp;
		list_add_tail(&funcpatch->list, &binpatch->functions);
	}

	list_for_each_entry(funcpatch, &binpatch->functions, list) {
		pr_debug("Function patch \"%s\"\n", funcpatch->fp->name);

		err = apply_funcpatch(ctx, funcpatch->addr, funcpatch->fp);
		if (err)
			break;
	}

	return err;
}

static int discover_plt_hints(struct process_ctx_s *ctx, const BinPatch *bp)
{
	int i, err;
	void *handle;
	LIST_HEAD(vmas);

	pr_info("= Map %s:\n", bp->new_path);

	pr_debug("  - Dlopen %s\n", bp->new_path);
	handle = dlopen(bp->new_path, RTLD_NOW);
	if (!handle) {
		pr_err("failed to dlopen %s: %s\n",bp->new_path, dlerror());
		return 1;
	}

	if (collect_vmas(getpid(), &vmas)) {
		pr_err("Can't collect local mappings\n");
		goto err;
	}

	pr_info("= Discovering target PLT hints:\n");

	for (i = 0; i < bp->n_relocations; i++) {
		RelaPlt *rp = bp->relocations[i];
		unsigned long addr;
		const struct vma_area *vma;

		if (rp->addend) {
			// Local symbol. Skip.
			continue;
		}

		if (rp->hint) {
			// Already discovered symbol in previous version of the
			// library. Skip.
			continue;
		}

		pr_info("  - searching external symbol '%s':\n", rp->name);

		addr = (unsigned long)dlsym(handle, rp->name);
		if (!addr) {
			pr_err("failed to find symbol %s: %s\n", rp->name, dlerror());
			return 1;
		}

		vma = find_vma_by_addr(&vmas, addr);
		if (!vma) {
			pr_err("failed to find local vma with address %#lx\n", addr);
			goto err;
		}

		rp->hint = addr - vma->start;
		rp->path = vma->path;

		pr_info("     file   : %s\n", rp->path);
		pr_info("     offset : %#lx\n", rp->hint);
	}
	// TODO: need to:
	// 1) get all external symbol addresses
	// 2) get libraries by addresses.
	// 3) make sure, that all these libraries are mapped within the process by names
	// 4) make sure, that inodes of external libraries in nsb are equal to corresponding in the process.
	// 5) discover symbols offsets within mappings.
	// 6) use them to patch the new library.
	err = 0;
err:
	dlclose(handle);
	return err;
}

static int apply_rela_plt(struct process_ctx_s *ctx, const BinPatch *bp)
{
	int i;
	int err;
	int64_t load_addr = ctx->new_base;

	pr_info("= Applying destination PLT relocations:\n");

	for (i = 0; i < bp->n_relocations; i++) {
		RelaPlt *rp = bp->relocations[i];
		uint64_t plt_addr;
		uint64_t func_addr;

		pr_info("  - Entry \"%s\" (%s at %#x):\n",
			 rp->name, rp->info_type, rp->offset);

		plt_addr = load_addr + rp->offset;

		if (rp->addend) {
			pr_info("      Locality      : internal\n");
			func_addr = load_addr + rp->addend;
			pr_info("      Object address: %#lx (%#lx + %#x)\n",
					func_addr, load_addr, rp->addend);
		} else {
			const struct vma_area *vma;

			pr_info("      Locality      : external\n");

			vma = find_vma_by_path(&ctx->vmas, rp->path);
			if (!vma) {
				pr_err("failed to find %s map\n", rp->path);
				return -EINVAL;
			}
			pr_info("      Mapped file   : %s at %#lx\n", vma->path, vma->start);
			pr_debug("    %s start: %#lx\n", rp->path, vma->start);
			func_addr = vma->start + rp->hint;
			pr_info("      Object address: %#lx (%#lx + %#lx)\n",
					func_addr, vma->start, rp->hint);
		}

		pr_info("      PLT address   : %#lx\n", plt_addr);
		pr_info("        Overwrite .got.plt entry: %#lx ---> %#lx\n",
				func_addr, plt_addr);

		err = process_write_data(ctx->pid, plt_addr, &func_addr, sizeof(func_addr));
		if (err) {
			pr_err("failed to write to addr %#lx in process %d\n",
					plt_addr, ctx->pid);
			return err;
		}
	}
	return 0;
}

static int fix_dyn_entry(struct process_ctx_s *ctx, BinPatch *bp, FuncPatch *fp)
{
	unsigned char jump[X86_MAX_SIZE];
	unsigned long old_addr, new_addr;
	ssize_t size;
	int err;

	pr_info("  - Entry \"%s\":\n", fp->name);
	if (!fp->has_old_addr) {
		pr_debug("   New function. Skip\n");
		return 0;
	}

	old_addr = ctx->old_base + fp->old_addr;
	new_addr = ctx->new_base + fp->addr;

	pr_info("      old address: %#lx\n", old_addr);
	pr_info("      new address: %#lx\n", new_addr);

	pr_info("        jump: %#lx ---> %#lx\n", old_addr, new_addr);

	size = x86_jmpq_instruction(jump, old_addr, new_addr);
	if (size < 0)
		return size;

	err = process_write_data(ctx->pid, old_addr, jump, round_up(size, 8));
	if (err < 0) {
		pr_err("failed to patch: %d\n", err);
		return err;
	}
	return 0;
}

static int process_copy_lw(pid_t pid, unsigned long dest, unsigned long src)
{
	int err;
	char buf[8];

	err = process_read_data(pid, src, buf, 8);
	if (err) {
		pr_err("failed to read from %lx\n", src);
		return err;
	}
	err = process_write_data(pid, dest, buf, 8);
	if (err) {
		pr_err("failed to write to %lx\n", dest);
		return err;
	}
	return 0;
}

static int process_copy_data(pid_t pid, unsigned long dst, unsigned long src, size_t size)
{
	int iter = size / 8;
	int remain = size % 8;
	int err, i;

	for (i = 0; i < iter; i++) {
		err = process_copy_lw(pid, src, dst);
		if (err) {
			pr_err("failed to copy from %#lx to %#lx\n", src, dst);
			return err;
		}
		src += 8;
		dst += 8;
	}
	if (remain) {
		char buf[8], tmp[8];

		err = process_read_data(pid, dst, buf, 8);
		if (err) {
			pr_err("failed to read from %lx\n", dst);
			return err;
		}

		err = process_read_data(pid, src, tmp, 8);
		if (err) {
			pr_err("failed to read from %lx\n", src);
			return err;
		}
		memcpy(buf, tmp, remain);
		err = process_write_data(pid, dst, buf, 8);
		if (err) {
			pr_err("failed to write to %lx\n", dst);
			return err;
		}
	}
	return 0;
}

static int copy_local_data(struct process_ctx_s *ctx, BinPatch *bp)
{
	int i;

	// TODO: either all (!) functions have to be redirected from old library to new one,
	// or at least all the functions, accessing local data.
	// Otherwise some of the functions in the old library can modify old copy of the data, 
	// while some of the functions in the new library will modify new copy.
	pr_info("= Copy global variables:\n");
	for (i = 0; i < bp->n_local_vars; i++) {
		DataSym *ds = bp->local_vars[i];
		unsigned long from, to;
		int err;

		from = ctx->old_base + ds->ref;
		to = ctx->new_base + ds->offset;

		pr_info("  - %s (size: %d): %#lx ---> %#lx\n",
				ds->name, ds->size, from, to);
		err = process_copy_data(ctx->pid, to, from, ds->size);
		if (err)
			return err;
	}
	return 0;
}

static int set_dyn_jumps(struct process_ctx_s *ctx)
{
	int i, err;
	struct binpatch_s *binpatch = &ctx->binpatch;
	BinPatch *bp = binpatch->bp;

	pr_info("= Apply jumps:\n");
	for (i = 0; i < bp->n_patches; i++) {
		FuncPatch *fp = bp->patches[i];

		if (fp->dyn) {
			err = fix_dyn_entry(ctx, bp, fp);
			if (err)
				return err;
		}
	}
	return 0;
}

static int vma_fix_target_syms(struct process_ctx_s *ctx, const struct vma_area *vma)
{
	struct extern_symbol *es;
	int64_t address;
	int err;
	struct elf_info_s *ei;

	ei = elf_create_info(ctx->binpatch.bp->new_path);
	if (!ei)
		return -1;

	list_for_each_entry(es, &vma->target_syms, list) {
		unsigned long offset = es->offset;

		pr_debug("    \"%s\":\n", es->name);

		if (elf_type_dyn(vma->ei))
			offset += vma->start;

		pr_debug("       GOT address: %#lx\n", offset);

		address = elf_dsym_offset(ei, es->name);

		address += ctx->new_base;
		pr_debug("       new address: %#lx\n", address);

		pr_info("          Overwrite .got.plt entry: %#lx ---> %#lx\n", address, es->offset);
		err = process_write_data(ctx->pid, es->offset, &address, sizeof(address));
		if (err < 0) {
			pr_err("failed to patch: %d\n", err);
			return err;
		}
	}

	return 0;
}

static int fix_vma_refs(struct vma_area *vma, void *data)
{
	struct process_ctx_s *ctx = data;

	if (list_empty(&vma->target_syms))
		return 0;

	pr_debug("  - %s -> %s:\n", vma->map_file, vma->path);
	return vma_fix_target_syms(ctx, vma);
}

static int fix_target_references(struct process_ctx_s *ctx)
{
	pr_info("= Fix target references:\n");
	return iterate_file_vmas(&ctx->vmas, ctx, fix_vma_refs);
}

static int apply_dyn_binpatch(struct process_ctx_s *ctx)
{
	struct binpatch_s *binpatch = &ctx->binpatch;
	BinPatch *bp = binpatch->bp;
	int err;

	err = discover_plt_hints(ctx, bp);
	if (err)
		return err;

	ctx->new_base = load_elf(ctx, bp, ctx->pvma->start);
	if (ctx->new_base < 0)
		return ctx->new_base;

	err = apply_rela_plt(ctx, bp);
	if (err)
		return err;

	err = copy_local_data(ctx, bp);
	if (err)
		return err;

	if (ctx->ops->set_jumps) {
		err = ctx->ops->set_jumps(ctx);
		if (err)
			return err;
	}

	err = fix_target_references(ctx);
	if (err)
		return err;

	if (ctx->ops->cleanup_target) {
		err = ctx->ops->cleanup_target(ctx);
		if (err)
			return err;
	}

	return 0;
}

struct ctx_dep {
	struct list_head	list;
	const struct vma_area	*vma;
};

static struct ctx_dep *ctx_create_dep(const struct vma_area *vma)
{
	struct ctx_dep *cd;

	cd = xmalloc(sizeof(*cd));
	if (!cd)
		return NULL;
	cd->vma = vma;
	return cd;
}

static int collect_lib_deps(struct process_ctx_s *ctx, const struct vma_area *vma, struct list_head *head)
{
	struct elf_needed *en;
	LIST_HEAD(needed);
	LIST_HEAD(nested);

	list_for_each_entry(en, elf_needed_list(vma->ei), list) {
		const struct vma_area *vma;
		struct ctx_dep *cd;
		int err;

		vma = find_vma_by_soname(&ctx->vmas, en->needed);
		if (!vma) {
			pr_err("failed to find VMA by soname %s\n", en->needed);
			return -ENOENT;
		}

		cd = ctx_create_dep(vma);
		if (!cd)
			return -EINVAL;
		list_add_tail(&cd->list, &needed);

		err = collect_lib_deps(ctx, vma, &nested);
		if (err)
			return err;

	}
	list_splice_tail(&needed, head);
	list_splice_tail(&nested, head);
	return 0;
}

static int collect_ctx_deplist(struct process_ctx_s *ctx)
{
	struct ctx_dep *cd;
	char path[PATH_MAX];
	char *exe_bid;
	const struct vma_area *exe_vma;

	snprintf(path, sizeof(path), "/proc/%d/exe", ctx->pid);

	exe_bid = elf_build_id(path);
	if (!exe_bid)
		return -EINVAL;

	exe_vma = find_vma_by_bid(&ctx->vmas, exe_bid);
	if (!exe_vma)
		return -EINVAL;

	cd = ctx_create_dep(exe_vma);
	if (!cd)
		return -EINVAL;
	list_add_tail(&cd->list, &ctx->objdeps);

	return collect_lib_deps(ctx, exe_vma, &ctx->objdeps);
}

static int get_ctx_deplist(struct process_ctx_s *ctx)
{
	int err;
	struct ctx_dep *cd;

	pr_debug("= Process soname search list:\n");

	err = collect_ctx_deplist(ctx);
	if (err)
		return err;

	list_for_each_entry(cd, &ctx->objdeps, list)
		pr_debug("      - %s - %s\n", vma_soname(cd->vma), cd->vma->path);

	return 0;
}

static int ctx_bind_es(const struct process_ctx_s *ctx, struct extern_symbol *es)
{
	int ret;
	const struct ctx_dep *n;

	list_for_each_entry(n, &ctx->objdeps, list) {
		const struct vma_area *vma = n->vma;

		ret = elf_contains_sym(vma->ei, es->name);
		if (ret < 0)
			return ret;
		if (ret) {
			es->vma = vma;
			return 0;
		}
	}

	if (elf_weak_sym(es))
		return 0;

	pr_err("failed to find %s symbol\n", es->name);
	return -ENOENT;
}

static int collect_dependent_vma(struct process_ctx_s *ctx,
				 struct vma_area *vma)
{
	int err;
	LIST_HEAD(plt_syms);
	struct extern_symbol *es, *tmp;

	err = elf_extern_dsyms(vma->ei, &plt_syms);
	if (err)
		return err;

	pr_debug("    PLT symbols to update:\n");
	list_for_each_entry_safe(es, tmp, &plt_syms, list) {
		err = ctx_bind_es(ctx, es);
		if (err)
			return err;

		if (es->vma != ctx->pvma)
			continue;

		list_move(&es->list, &vma->target_syms);

		pr_debug("      - %s:\n", es->name);
		pr_debug("          offset  : %#lx\n", es->offset);
		pr_debug("          bind    : %d\n", es->bind);
		pr_debug("          path    : %s\n", es->vma->path);
		pr_debug("          map_file: %s\n", es->vma->map_file);
	}

	return 0;
}

static int check_file_vma(struct vma_area *vma, void *data)
{
	struct process_ctx_s *ctx = data;
	int ret;

	if (!vma->path)
		return 0;

	if (!vma_is_executable(vma))
		return 0;

	pr_info("  - %#lx-%#lx -> %s\n", vma->start, vma->end, vma->path);
	ret = elf_soname_needed(vma->ei, vma_soname(ctx->pvma));
	if (ret < 0)
		pr_err("failed to find %s dependences: %d\n", vma->path, ret);
	else if (ret)
		ret = collect_dependent_vma(ctx, vma);
	return ret < 0 ? ret : 0;
}

static int process_collect_dependable_vmas(struct process_ctx_s *ctx)
{
	if (!elf_type_dyn(ctx->pvma->ei))
		return 0;

	pr_info("= Searching mappings, depending on \"%s\":\n", vma_soname(ctx->pvma));
	return iterate_file_vmas(&ctx->vmas, ctx, check_file_vma);
}

static int process_find_patchable_vma(struct process_ctx_s *ctx, BinPatch *bp)
{
	const struct vma_area *pvma;

	pr_info("= Searching source VMA:\n");

	pvma = find_vma_by_bid(&ctx->vmas, bp->old_bid);
	if (!pvma) {
		pr_err("failed to find process %d vma with Build ID %s\n",
				ctx->pid, bp->old_bid);
		return -ENOENT;
	}
	pr_info("  - path   : %s\n", pvma->path);
	pr_info("  - address: %#lx\n", pvma->start);
	ctx->pvma = pvma;
	ctx->old_base = ctx->pvma->start;
	return 0;
}

static int init_context(struct process_ctx_s *ctx, pid_t pid,
			const char *patchfile, const char *how)
{
	struct binpatch_s *binpatch = &ctx->binpatch;
	BinPatch *bp;

	pr_info("Patch context:\n");
	pr_info("  Pid        : %d\n", pid);

	ctx->pid = pid;
	INIT_LIST_HEAD(&ctx->vmas);
	INIT_LIST_HEAD(&ctx->objdeps);
	INIT_LIST_HEAD(&ctx->threads);

	INIT_LIST_HEAD(&binpatch->functions);
	INIT_LIST_HEAD(&binpatch->places);

	bp = read_binpatch(patchfile);
	if (!bp)
		return -1;

	ctx->ops = set_patch_ops(how, bp->object_type);
	if (!ctx->ops)
		goto err;

	pr_info("  source BID : %s\n", bp->old_bid);
	pr_info("  target path: %s\n", bp->new_path);
	pr_info("  object type: %s\n", bp->object_type);
	pr_info("  patch mode : %s\n", ctx->ops->name);

	if (collect_vmas(ctx->pid, &ctx->vmas)) {
		pr_err("Can't collect mappings for %d\n", ctx->pid);
		goto err;
	}

	if (process_find_patchable_vma(ctx, bp))
		goto err;

	if (get_ctx_deplist(ctx))
		goto err;

	if (process_collect_dependable_vmas(ctx)) {
		pr_err("failed to find dependable VMAs\n");
		goto err;
	}

	binpatch->bp = bp;
	return 0;

err:
	bin_patch__free_unpacked(bp, NULL);
	return -1;
}

static int process_resume(struct process_ctx_s *ctx)
{
	pr_info("= Resuming %d\n", ctx->pid);
	return process_cure(ctx);
}

static int process_call_in_map(const struct list_head *calls,
			       uint64_t map_start, uint64_t map_end)
{
	struct backtrace_function_s *bf;

	list_for_each_entry(bf, calls, list) {
		if ((map_start < bf->ip) && (bf->ip < map_end)) {
			pr_debug("Found call in stack within "
				 "patching range: %#lx (%#lx-%lx)\n",
				 bf->ip, map_start, map_end);
			return 1;
		}
	}
	return 0;
}

static int jumps_check_backtrace(const struct process_ctx_s *ctx,
				 const struct backtrace_s *bt)
{
	const struct binpatch_s *binpatch = &ctx->binpatch;
	const BinPatch *bp = binpatch->bp;
	int i;

	for (i = 0; i < bp->n_patches; i++) {
		FuncPatch *fp = bp->patches[i];
		uint64_t start, end;

		/* Skip new functions: they are outside stack by default */
		if (fp->new_)
			continue;

		start = ctx->old_base + fp->addr;
		end = start + fp->size;

		pr_debug("      Patch: %#lx - %#lx\n", start, end);
		if (process_call_in_map(&bt->calls, start, end))
			return -EAGAIN;
	}
	return 0;
}

static int process_catch(struct process_ctx_s *ctx)
{
	int ret, err;

	err = process_infect(ctx);
	if (err)
		return err;

	ret = process_check_stack(ctx, ctx->ops->check_backtrace);
	if (ret)
		goto err;

	return 0;

err:
	err = process_cure(ctx);
	return ret ? ret : err;
}

static int process_suspend(struct process_ctx_s *ctx)
{
	int try = 0, tries = 25;
	int timeout_msec = 20;
	int err;

	do {
		if (try) {
			pr_info("Failed to catch process in a suitable time/place.\n"
				"Retry in %d msec\n", timeout_msec);
			usleep(timeout_msec * 1000);
			timeout_msec <<= 1;
		}
		err = process_catch(ctx);
		if (err != -EAGAIN)
			break;
	} while (++try < tries);

	return err == -EAGAIN ? -ETIME : err;
}

int patch_process(pid_t pid, const char *patchfile, const char *how)
{
	int ret, err;
	struct process_ctx_s *ctx = &process_context;

	err = init_context(ctx, pid, patchfile, how);
	if (err)
		return err;

	err = process_suspend(ctx);
	if (err) {
		errno = -err;
		pr_perror("Failed to suspend process");
		return err;
	}

	ret = process_link(ctx);
	if (ret)
		goto resume;

	ret = ctx->ops->apply_patch(ctx);
	if (ret)
		 pr_err("failed to apply binary patch\n");

	/* TODO all the work has to be rolled out, if an error occured */
resume:
	err = process_resume(ctx);

	pr_info("Done\n");
	return ret ? ret : err;
}

int check_process(pid_t pid, const char *patchfile)
{
	int err;
	LIST_HEAD(vmas);
	BinPatch *bp;

	err = collect_vmas(pid, &vmas);
	if (err) {
		pr_err("Can't collect mappings for %d\n", pid);
		return err;
	}

	bp = read_binpatch(patchfile);
	if (!bp)
		return -1;

	return !find_vma_by_bid(&vmas, bp->old_bid);
}

struct patch_ops_s patch_jump_ops = {
	.name = "jump",
	.set_jumps = set_dyn_jumps,
	.check_backtrace = jumps_check_backtrace,
	.cleanup_target = NULL,
};

static int unmap_file_vma(struct vma_area *vma, void *data)
{
	struct process_ctx_s *ctx = data;

	if (!vma->path)
		return 0;

	if (strcmp(vma->path, ctx->pvma->path))
		return 0;

	return process_unmap(ctx, vma->start, vma->end - vma->start);
}

static int unmap_old_lib(struct process_ctx_s *ctx)
{
	if (fixup_rtld(ctx))
		return -EINVAL;

	pr_debug("= Unmap target VMA:\n");
	return iterate_file_vmas(&ctx->vmas, ctx, unmap_file_vma);
}

static int swap_check_backtrace(const struct process_ctx_s *ctx,
				const struct backtrace_s *bt)
{
	if (bt->depth == 1)
		return -EAGAIN;

	if (process_call_in_map(&bt->calls, ctx->pvma->start, ctx->pvma->end))
		return -EAGAIN;
	return 0;
}

struct patch_ops_s patch_swap_ops = {
	.name = "swap",
	.set_jumps = NULL,
	.check_backtrace = swap_check_backtrace,
	.cleanup_target = unmap_old_lib,
};

static struct patch_ops_s *get_patch_ops(const char *how)
{
	if (!strcmp(how, "jump"))
		return &patch_jump_ops;
	if (!strcmp(how, "swap"))
		return &patch_swap_ops;
	pr_msg("Error: \"how\" option can be either \"jump\" or \"swap\"\n");
	return NULL;
}

static const struct patch_ops_s *set_patch_ops(const char *how, const char *type)
{
	struct patch_ops_s *ops;
	int (*apply)(struct process_ctx_s *ctx);

	if (!strcmp(type, "ET_EXEC")) {
		how = "jump";
		apply = apply_exec_binpatch;
	} else if (!strcmp(type, "ET_DYN"))
		apply = apply_dyn_binpatch;
	else {
		pr_err("Unknown patch type: %s\n", type);
		return NULL;
	}

	ops = get_patch_ops(how);
	ops->apply_patch = apply;
	return ops;
}

int check_patch_mode(const char *how)
{
	return get_patch_ops(how) ? 0 : -EINVAL;
}
