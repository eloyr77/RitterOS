#!/usr/bin/env python3
"""The make test's cases (59): Makefiles and the steps run on them.

  maketest.py files DIR       write every case's files into DIR/NAME/
  maketest.py steps           print "NAME<TAB>INDEX<TAB>STEP" per step (MAKE: the make)
  maketest.py expected DIR    run every case with the Mac's GNU make in DIR/NAME/ and
                              write each step's output (stdout and stderr together,
                              `x' quotes as 'x') to DIR/expected/NAME.INDEX

The guest runs the same steps in /test/m/NAME/ with RitterOS's make; the test
compares their output with GNU make's.
"""
import os
import shutil
import subprocess
import sys

# (name, {file: content}, [steps]); a step is a shell command, MAKE the make.
T = "\t"
cases = [
("basic", {"Makefile": f"all: hello\n\nhello: hello.txt\n{T}cat hello.txt > hello\n{T}echo built $@ from $<\n", "hello.txt": "hi\n"},
 ["MAKE", "MAKE", "cat hello"]),
("phony", {"Makefile": f".PHONY: clean all\nall:\n{T}@echo all ran\nclean:\n{T}@echo cleaning\n"}, ["MAKE", "MAKE clean", "MAKE all clean"]),
("vars", {"Makefile": "A = one\nB := $(A) two\nA = changed\nC = $(A)\nD ?= dflt\nD ?= other\nE = e1\nE += e2\nall:\n\t@echo '$(B)|$(C)|$(D)|$(E)'\n"}, ["MAKE", "MAKE A=cmd", "D=env MAKE"]),
("funcs", {"Makefile": """SRCS := a.c b.c dir/c.c
OBJS := $(SRCS:.c=.o)
P := $(patsubst %.c,%.o,$(SRCS))
all:
\t@echo $(OBJS)
\t@echo $(P)
\t@echo $(notdir $(SRCS)) $(dir $(SRCS))
\t@echo $(basename $(SRCS)) $(suffix $(SRCS))
\t@echo $(addprefix x/,a b) $(addsuffix .h,a b)
\t@echo $(subst a,A,banana) [$(strip   a   b  )]
\t@echo $(filter %.c,a.c b.h c.c) $(filter-out %.c,a.c b.h c.c)
\t@echo $(sort b a c a) $(words a b c) $(word 2,a b c) $(firstword x y) $(lastword x y)
\t@echo $(wordlist 2,3,a b c d) $(join a b,1 2) $(findstring an,banana)
\t@echo $(if x,yes,no) $(if ,yes,no) $(or ,b) $(and a,b)
\t@echo $(foreach w,a b c,[$(w)])
\t@echo $(origin SRCS) $(origin CC) $(origin UNDEFINEDX)
"""}, ["MAKE"]),
("call", {"Makefile": "reverse = $(2) $(1)\npair = [$(1),$(2)]\nall:\n\t@echo $(call reverse,a,b) $(call pair,x,y)\n"}, ["MAKE"]),
("shell", {"Makefile": "LIST := $(shell echo one; echo two)\nall:\n\t@echo [$(LIST)]\n"}, ["MAKE"]),
("wildcard", {"Makefile": "FILES := $(wildcard *.txt)\nALL := $(wildcard sub/*.h)\nall:\n\t@echo $(FILES) / $(ALL) / $(wildcard nothing*)\n",
  "b.txt": "", "a.txt": "", "c.dat": "", "sub/x.h": "", "sub/y.h": ""}, ["MAKE"]),
("cond", {"Makefile": """X = yes
ifeq ($(X),yes)
A = 1
else
A = 2
endif
ifneq "$(X)" "no"
B = 3
endif
ifdef X
C = 4
endif
ifndef UNDEF
D = 5
else
D = 6
endif
ifeq ($(X),no)
E = 7
else ifeq ($(X),yes)
E = 8
else
E = 9
endif
all:
\t@echo $(A)$(B)$(C)$(D)$(E)
"""}, ["MAKE", "MAKE X=no"]),
("pattern", {"Makefile": f"all: a.out b.out\n%.out: %.in\n{T}cp $< $@\n", "a.in": "A\n", "b.in": "B\n"}, ["MAKE", "MAKE", "sleep 1", "touch a.in", "MAKE"]),
("static", {"Makefile": f"OBJS = x.o y.o\nall: $(OBJS)\n$(OBJS): %.o: %.src\n{T}@echo $@ from $< stem $*\n{T}touch $@\n", "x.src": "", "y.src": ""}, ["MAKE"]),
("suffix", {"Makefile": f".SUFFIXES: .up .dn\n.up.dn:\n{T}tr a-z A-Z < $< > $@\nall: w.dn\n", "w.up": "hello\n"}, ["MAKE", "cat w.dn"]),
("autovars", {"Makefile": f"t: a b a c\n{T}@echo '@=$@ <=$< ^=$^ +=$+'\na b c:\n{T}@touch $@\n"}, ["MAKE"]),
("newer", {"Makefile": f"out: a b\n{T}@echo newer: $?\n{T}@touch out\n", "a": "", "b": ""}, ["MAKE", "sleep 1", "touch b", "MAKE"]),
("dirs", {"Makefile": f"build/x.o: src/x.c\n{T}@mkdir -p $(@D)\n{T}@echo $(@D) $(@F) $(<D) $(<F)\n{T}@touch $@\n", "src/x.c": ""}, ["MAKE"]),
("silent", {"Makefile": f"all:\n{T}echo loud\n{T}@echo quiet\n"}, ["MAKE", "MAKE -s", "MAKE -n"]),
("errors", {"Makefile": f"all: one two\none:\n{T}@echo one\n{T}false\n{T}@echo never\ntwo:\n{T}@echo two\n"}, ["MAKE; echo status $?", "MAKE -k; echo status $?"]),
("ignore", {"Makefile": f"all:\n{T}-false\n{T}@echo after\n"}, ["MAKE; echo status $?", "MAKE -i; echo status $?"]),
("norule", {"Makefile": "all: missing.txt\n"}, ["MAKE; echo status $?"]),
("include", {"Makefile": "include defs.mk\n-include nothere.mk\nall:\n\t@echo $(FROM_INCLUDE)\n", "defs.mk": "FROM_INCLUDE = included\n"}, ["MAKE"]),
("recursive", {"Makefile": "all:\n\t@$(MAKE) --no-print-directory -C sub X=passed\n", "sub/Makefile": "all:\n\t@echo in sub: $(X)\n"}, ["MAKE"]),
("define", {"Makefile": "define TWO\n@echo first\n@echo second\nendef\nall:\n\t$(TWO)\n"}, ["MAKE"]),
("override", {"Makefile": "override V = file\nW = file\nall:\n\t@echo $(V) $(W)\n"}, ["MAKE V=cmd W=cmd"]),
("eval", {"Makefile": "define RULE\n$(1):\n\t@echo rule for $(1)\nendef\n$(foreach t,p q,$(eval $(call RULE,$(t))))\nall: p q\n"}, ["MAKE"]),
("uptodate", {"Makefile": f"f: g\n{T}cp g f\n", "g": "x\n"}, ["MAKE", "MAKE", "MAKE -q; echo q $?"]),
("nothing", {"Makefile": "all:\n"}, ["MAKE"]),
("multiline", {"Makefile": f"all:\n{T}@for i in 1 2 3; do \\\n{T}  echo n$$i; \\\n{T}done\n"}, ["MAKE"]),
("continued", {"Makefile": "LIST = a \\\n       b \\\n       c\nall:\n\t@echo [$(LIST)]\n"}, ["MAKE"]),
("comments", {"Makefile": "# a comment\nX = value # trailing\nY = has\\#hash\nall: # comment on a rule\n\t@echo [$(X)] [$(Y)]\n"}, ["MAKE"]),
("export", {"Makefile": "export GREETING = hello\nall:\n\t@echo $$GREETING\n"}, ["MAKE"]),
("dollar", {"Makefile": "all:\n\t@echo $$HOME_X is $$ and $$(echo sub)\n"}, ["MAKE"]),
("targetvar", {"Makefile": "A = global\nB = $(A)\nfoo: A = special\nfoo:\n\t@echo foo: $(A) $(B)\nbar:\n\t@echo bar: $(A) $(B)\n"}, ["MAKE foo bar"]),
("ccrule", {"Makefile": "all: prog.o\n", "prog.c": ""}, ["MAKE -n"]),
("defgoal", {"Makefile": ".PHONY: first\nfirst:\n\t@echo first is default\nsecond:\n\t@echo second\n"}, ["MAKE", "MAKE second"]),
("orderonly", {"Makefile": f"out/f: in | out\n{T}@cp in out/f; echo copied\nout:\n{T}@mkdir -p out; echo made dir\n", "in": "x\n"}, ["MAKE", "MAKE"]),
("force", {"Makefile": f"x: FORCE\n{T}@echo forced\nFORCE:\n"}, ["MAKE", "MAKE"]),
("doublecolon_dash", {"Makefile": "N = 3\nall:\n\t@echo $(words $(wildcard *.mk)) $(N)\n"}, ["MAKE"]),
("recipeprefix", {"Makefile": f"all: ; @echo inline recipe\n"}, ["MAKE"]),
("flavor", {"Makefile": "A = x\nB := y\nall:\n\t@echo $(flavor A) $(flavor B) $(flavor C) $(value A)\n"}, ["MAKE"]),
]


def write_files(base, name, files):
    d = os.path.join(base, name)
    shutil.rmtree(d, ignore_errors=True)
    for f, content in files.items():
        p = os.path.join(d, f)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "w") as out:
            out.write(content)
    return d


def main():
    if len(sys.argv) >= 3 and sys.argv[1] == "files":
        for name, files, _ in cases:
            write_files(sys.argv[2], name, files)
    elif len(sys.argv) == 2 and sys.argv[1] == "steps":
        for name, _, steps in cases:
            for i, step in enumerate(steps):
                print(f"{name}\t{i}\t{step}")
    elif len(sys.argv) == 3 and sys.argv[1] == "expected":
        base = sys.argv[2]
        out_dir = os.path.join(base, "expected")
        os.makedirs(out_dir, exist_ok=True)
        env = {k: v for k, v in os.environ.items() if k not in ("MAKEFLAGS", "MAKELEVEL", "MFLAGS")}
        for name, files, steps in cases:
            d = write_files(os.path.join(base, "run"), name, files)
            for i, step in enumerate(steps):
                r = subprocess.run(step.replace("MAKE", "/usr/bin/make"), shell=True, cwd=d, env=env,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
                text = r.stdout.replace("`", "'")
                last = step.split(";")[-1].strip()
                if r.returncode and last.startswith("MAKE"):
                    text += f"make: exit code {r.returncode}\n"
                with open(os.path.join(out_dir, f"{name}.{i}"), "w") as f:
                    f.write(text)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
