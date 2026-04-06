/*
** $Id: lparser.c $
** Lua Parser
** See Copyright Notice in lua.h
*/

#define lparser_c
#define LUA_CORE

#include "lprefix.h"


#include <limits.h>
#include <string.h>

#include "lua.h"

#include "lcode.h"
#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "llex.h"
#include "lmem.h"
#include "lobject.h"
#include "lopcodes.h"
#include "lparser.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"



/* maximum number of variable declarations per function (must be
   smaller than 250, due to the bytecode format) */
#define MAXVARS		200


#define hasmultret(k)		((k) == VCALL || (k) == VVARARG)


/* because all strings are unified by the scanner, the parser
   can use pointer equality for string equality */
#define eqstr(a,b)	((a) == (b))


/*
** nodes for block list (list of active blocks)
*/
typedef struct BlockCnt {
  struct BlockCnt *previous;  /* chain */
  int firstlabel;  /* index of first label in this block */
  int firstgoto;  /* index of first pending goto in this block */
  short nactvar;  /* number of active declarations at block entry */
  lu_byte upval;  /* true if some variable in the block is an upvalue */
  lu_byte isloop;  /* 1 if 'block' is a loop; 2 if it has pending breaks */
  lu_byte insidetbc;  /* true if inside the scope of a to-be-closed var. */
} BlockCnt;



/*
** prototypes for recursive non-terminal functions
*/
static void statement (LexState *ls);
static void expr (LexState *ls, expdesc *v);


/*
** =======================================================
** Type annotation parsing and compile-time type checking
** =======================================================
*/


/* built-in type names recognized by the type system */
static const char *const builtin_types[] = {
  "number", "string", "boolean", "table", "function",
  "nil", "thread", "userdata", "any", "unknown", NULL
};


/*
** Map type name to numeric TYPEID for OP_TYPECHECK fast path.
** Returns -1 if not a built-in type (i.e., it's a class name).
*/
static int get_typeid (const char *typename_) {
  if (strcmp(typename_, "number") == 0) return TYPEID_NUMBER;
  if (strcmp(typename_, "string") == 0) return TYPEID_STRING;
  if (strcmp(typename_, "boolean") == 0) return TYPEID_BOOLEAN;
  if (strcmp(typename_, "table") == 0) return TYPEID_TABLE;
  if (strcmp(typename_, "function") == 0) return TYPEID_FUNCTION;
  if (strcmp(typename_, "nil") == 0) return TYPEID_NIL;
  if (strcmp(typename_, "thread") == 0) return TYPEID_THREAD;
  if (strcmp(typename_, "userdata") == 0) return TYPEID_USERDATA;
  if (strcmp(typename_, "any") == 0) return TYPEID_ANY;
  if (strcmp(typename_, "unknown") == 0) return TYPEID_UNKNOWN;
  return -1;  /* class type */
}


/* access modifier codes */
#define ACCESS_PRIVATE    1
#define ACCESS_PROTECTED  2
#define ACCESS_READONLY   3


/*
** Register a class field's access modifier for compile-time checking.
*/
static void register_classfield (LexState *ls, TString *classname,
                                  TString *fieldname, lu_byte access) {
  lua_State *L = ls->L;
  if (ls->nclassfields >= ls->classfields_size) {
    int newsize = (ls->classfields_size == 0) ? 16 : ls->classfields_size * 2;
    ls->classfields = luaM_reallocvector(L, ls->classfields,
                        ls->classfields_size, newsize,
                        struct ClassFieldAccess);
    ls->classfields_size = newsize;
  }
  struct ClassFieldAccess *cf = &ls->classfields[ls->nclassfields++];
  cf->classname = classname;
  cf->fieldname = fieldname;
  cf->access = access;
}


/*
** Check at compile time if accessing 'fieldname' on a variable of type
** 'classname' is allowed from the current function context.
** Returns 0 if OK, or the access code (1=private, 2=protected) if blocked.
*/
static int check_field_access (LexState *ls, TString *classname,
                                TString *fieldname) {
  int i;
  FuncState *fs = ls->fs;
  for (i = 0; i < ls->nclassfields; i++) {
    struct ClassFieldAccess *cf = &ls->classfields[i];
    if (cf->classname == classname && cf->fieldname == fieldname) {
      if (cf->access == ACCESS_PRIVATE) {
        /* private: only allowed inside methods of the same class */
        if (fs->classctx != classname)
          return ACCESS_PRIVATE;
      }
      else if (cf->access == ACCESS_PROTECTED) {
        /* protected: allowed in same class or parent class methods */
        /* (simplified: check if classctx is set and is the class or a parent) */
        if (fs->classctx == NULL)
          return ACCESS_PROTECTED;
        /* if classctx == classname, OK */
        if (fs->classctx != classname) {
          /* check if classctx's parent chain includes classname */
          /* for now, allow if any classctx is set (simplified) */
          /* TODO: walk parent chain */
        }
      }
      break;
    }
  }
  return 0;  /* access OK */
}


/*
** Register a class method for override checking.
*/
static void register_classmethod_info (LexState *ls, TString *classname,
                                        TString *methodname) {
  lua_State *L = ls->L;
  if (ls->nclassmethods >= ls->classmethods_size) {
    int newsize = (ls->classmethods_size == 0) ? 32 : ls->classmethods_size * 2;
    ls->classmethods = luaM_reallocvector(L, ls->classmethods,
                         ls->classmethods_size, newsize, struct ClassMethodInfo);
    ls->classmethods_size = newsize;
  }
  ls->classmethods[ls->nclassmethods].classname = classname;
  ls->classmethods[ls->nclassmethods].methodname = methodname;
  ls->nclassmethods++;
}

static void register_classparent (LexState *ls, TString *classname,
                                   TString *parentname) {
  lua_State *L = ls->L;
  if (ls->nclassparents >= ls->classparents_size) {
    int newsize = (ls->classparents_size == 0) ? 8 : ls->classparents_size * 2;
    ls->classparents = luaM_reallocvector(L, ls->classparents,
                         ls->classparents_size, newsize, struct ClassParentInfo);
    ls->classparents_size = newsize;
  }
  ls->classparents[ls->nclassparents].classname = classname;
  ls->classparents[ls->nclassparents].parentname = parentname;
  ls->nclassparents++;
}

/*
** Check if a method name exists in a class or its parent chain.
** Used for override validation.
*/
static int method_exists_in_parent (LexState *ls, TString *classname,
                                     TString *methodname) {
  /* find parent of classname */
  TString *parent = NULL;
  for (int i = 0; i < ls->nclassparents; i++) {
    if (ls->classparents[i].classname == classname) {
      parent = ls->classparents[i].parentname;
      break;
    }
  }
  if (parent == NULL) return 0;  /* no parent */
  /* check if method exists in parent */
  for (int i = 0; i < ls->nclassmethods; i++) {
    if (ls->classmethods[i].classname == parent &&
        ls->classmethods[i].methodname == methodname)
      return 1;
  }
  /* check parent's parent recursively */
  return method_exists_in_parent(ls, parent, methodname);
}


/*
** Register a class name in the parser's class registry.
*/
static void register_classname (LexState *ls, TString *name) {
  lua_State *L = ls->L;
  if (ls->nclasses >= ls->classnames_size) {
    int newsize = (ls->classnames_size == 0) ? 8 : ls->classnames_size * 2;
    ls->classnames = (TString **)luaM_reallocvector(L, ls->classnames,
                          ls->classnames_size, newsize, TString *);
    ls->classnames_size = newsize;
  }
  ls->classnames[ls->nclasses++] = name;
}


/*
** Check if a type name is valid (built-in type, declared class, or type alias).
** Returns 1 if valid, 0 if unknown.
*/
static int is_valid_typename (LexState *ls, TString *name) {
  const char *s = getstr(name);
  int i;
  /* check built-in types */
  for (i = 0; builtin_types[i] != NULL; i++) {
    if (strcmp(s, builtin_types[i]) == 0)
      return 1;
  }
  /* check declared classes (from 'class' keyword) */
  for (i = 0; i < ls->nclasses; i++) {
    if (ls->classnames[i] == name)  /* pointer equality (interned strings) */
      return 1;
  }
  /* check C-registered classes in registry (key = "class:Name") */
  {
    lua_State *L = ls->L;
    lua_pushfstring(L, "class:%s", s);
    lua_getfield(L, LUA_REGISTRYINDEX, lua_tostring(L, -1));
    int found = !lua_isnil(L, -1);
    lua_pop(L, 2);  /* pop key and value */
    if (found) return 1;
  }
  return 0;
}


/*
** Get the compile-time type of an expression kind.
** Returns a string like "number", "string", etc., or NULL if unknown.
*/
static const char *expr_compiletime_type (expkind k) {
  switch (k) {
    case VKINT: case VKFLT: return "number";
    case VKSTR: return "string";
    case VTRUE: case VFALSE: return "boolean";
    case VNIL: return "nil";
    default: return NULL;  /* type not known at compile time */
  }
}


/*
** Parse a type annotation after ':'. Consumes the ':' and the type name.
** Type syntax: NAME ['?'] | 'nil' | '{' ... '}'
** Returns the base type name as a TString* (for simple types like
** 'number', 'string', etc.) or NULL for complex types.
** Types are checked at runtime via OP_TYPECHECK for basic types.
*/
static TString *parse_type (LexState *ls) {
  /* parse_type -> NAME ['?'] | 'nil' | 'function' | '{' ... '}' | '(' ... ')' */
  TString *typename_ = NULL;
  int nullable = 0;
  switch (ls->t.token) {
    case TK_NAME: {
      typename_ = ls->t.seminfo.ts;  /* capture type name */
      luaX_next(ls);  /* skip type name (number, string, boolean, etc.) */
      /* generic type parameters: Name<T, U, ...> (parsed, no runtime check) */
      if (ls->t.token == '<') {
        luaX_next(ls);  /* skip '<' */
        parse_type(ls);  /* first type parameter */
        while (ls->t.token == ',') {
          luaX_next(ls);  /* skip ',' */
          parse_type(ls);  /* next type parameter */
        }
        if (ls->t.token != '>')
          luaX_syntaxerror(ls, "'>' expected to close generic type");
        luaX_next(ls);  /* skip '>' */
        typename_ = NULL;  /* generic types: skip validation/runtime check */
      }
      break;
    }
    case TK_NIL: {
      typename_ = luaX_newstring(ls, "nil", 3);
      luaX_next(ls);  /* skip 'nil' */
      break;
    }
    case TK_FUNCTION: {
      typename_ = luaX_newstring(ls, "function", 8);
      luaX_next(ls);  /* skip 'function' as type name */
      /* optional function signature: (params) -> rettype */
      if (ls->t.token == '(') {
        int depth = 1;
        luaX_next(ls);
        while (depth > 0 && ls->t.token != TK_EOS) {
          if (ls->t.token == '(') depth++;
          else if (ls->t.token == ')') depth--;
          if (depth > 0) luaX_next(ls);
        }
        luaX_next(ls);  /* skip ')' */
      }
      break;
    }
    case TK_TRUE: case TK_FALSE: {
      typename_ = luaX_newstring(ls, "boolean", 7);
      luaX_next(ls);  /* boolean literal types */
      break;
    }
    case '{': {
      /* table type: { ... } - skip balanced braces */
      typename_ = luaX_newstring(ls, "table", 5);
      int depth = 1;
      luaX_next(ls);  /* skip '{' */
      while (depth > 0 && ls->t.token != TK_EOS) {
        if (ls->t.token == '{') depth++;
        else if (ls->t.token == '}') depth--;
        if (depth > 0) luaX_next(ls);
      }
      luaX_next(ls);  /* skip final '}' */
      break;
    }
    case '(': {
      /* function type: (...) -> ... - skip balanced parens */
      typename_ = luaX_newstring(ls, "function", 8);
      int depth = 1;
      luaX_next(ls);  /* skip '(' */
      while (depth > 0 && ls->t.token != TK_EOS) {
        if (ls->t.token == '(') depth++;
        else if (ls->t.token == ')') depth--;
        if (depth > 0) luaX_next(ls);
      }
      luaX_next(ls);  /* skip final ')' */
      break;
    }
    default:
      luaX_syntaxerror(ls, "type name expected");
  }
  /* optional '?' for nullable types */
  if (ls->t.token == '?') {
    luaX_next(ls);  /* skip '?' */
    nullable = 1;
  }
  /* optional '|' for union types: type | type */
  if (ls->t.token == '|') {
    /* for union types, don't do runtime checking (too complex) */
    while (ls->t.token == '|') {
      luaX_next(ls);  /* skip '|' */
      parse_type(ls);  /* parse next type in union */
    }
    return NULL;  /* no single type to check */
  }
  /* 'any' type means no checking */
  if (typename_ != NULL && strcmp(getstr(typename_), "any") == 0)
    return NULL;  /* 'any' = no type checking */
  /* validate type name at compile time */
  if (typename_ != NULL && !is_valid_typename(ls, typename_))
    luaK_semerror(ls, "unknown type '%s'", getstr(typename_));
  (void)nullable;  /* nullable types allow nil at runtime (handled in VM) */
  return typename_;
}


/*
** Try to parse an optional type annotation (': type').
** Returns the type name TString* if annotation found, NULL otherwise.
*/
static TString *optional_type_annotation (LexState *ls) {
  if (ls->t.token == ':') {
    luaX_next(ls);  /* skip ':' */
    return parse_type(ls);
  }
  return NULL;
}


static l_noret error_expected (LexState *ls, int token) {
  luaX_syntaxerror(ls,
      luaO_pushfstring(ls->L, "%s expected", luaX_token2str(ls, token)));
}


static l_noret errorlimit (FuncState *fs, int limit, const char *what) {
  lua_State *L = fs->ls->L;
  const char *msg;
  int line = fs->f->linedefined;
  const char *where = (line == 0)
                      ? "main function"
                      : luaO_pushfstring(L, "function at line %d", line);
  msg = luaO_pushfstring(L, "too many %s (limit is %d) in %s",
                             what, limit, where);
  luaX_syntaxerror(fs->ls, msg);
}


void luaY_checklimit (FuncState *fs, int v, int l, const char *what) {
  if (l_unlikely(v > l)) errorlimit(fs, l, what);
}


/*
** Test whether next token is 'c'; if so, skip it.
*/
static int testnext (LexState *ls, int c) {
  if (ls->t.token == c) {
    luaX_next(ls);
    return 1;
  }
  else return 0;
}


/*
** Check that next token is 'c'.
*/
static void check (LexState *ls, int c) {
  if (ls->t.token != c)
    error_expected(ls, c);
}


/*
** Check that next token is 'c' and skip it.
*/
static void checknext (LexState *ls, int c) {
  check(ls, c);
  luaX_next(ls);
}


#define check_condition(ls,c,msg)	{ if (!(c)) luaX_syntaxerror(ls, msg); }


/*
** Check that next token is 'what' and skip it. In case of error,
** raise an error that the expected 'what' should match a 'who'
** in line 'where' (if that is not the current line).
*/
static void check_match (LexState *ls, int what, int who, int where) {
  if (l_unlikely(!testnext(ls, what))) {
    if (where == ls->linenumber)  /* all in the same line? */
      error_expected(ls, what);  /* do not need a complex message */
    else {
      luaX_syntaxerror(ls, luaO_pushfstring(ls->L,
             "%s expected (to close %s at line %d)",
              luaX_token2str(ls, what), luaX_token2str(ls, who), where));
    }
  }
}


static TString *str_checkname (LexState *ls) {
  TString *ts;
  check(ls, TK_NAME);
  ts = ls->t.seminfo.ts;
  luaX_next(ls);
  return ts;
}


static void init_exp (expdesc *e, expkind k, int i) {
  e->f = e->t = NO_JUMP;
  e->k = k;
  e->u.info = i;
}


static void codestring (expdesc *e, TString *s) {
  e->f = e->t = NO_JUMP;
  e->k = VKSTR;
  e->u.strval = s;
}


static void codename (LexState *ls, expdesc *e) {
  codestring(e, str_checkname(ls));
}


/*
** Register a new local variable in the active 'Proto' (for debug
** information).
*/
static short registerlocalvar (LexState *ls, FuncState *fs,
                               TString *varname) {
  Proto *f = fs->f;
  int oldsize = f->sizelocvars;
  luaM_growvector(ls->L, f->locvars, fs->ndebugvars, f->sizelocvars,
                  LocVar, SHRT_MAX, "local variables");
  while (oldsize < f->sizelocvars) {
    f->locvars[oldsize].varname = NULL;
    f->locvars[oldsize].typename_ = NULL;
    oldsize++;
  }
  f->locvars[fs->ndebugvars].varname = varname;
  f->locvars[fs->ndebugvars].typename_ = NULL;
  f->locvars[fs->ndebugvars].startpc = fs->pc;
  luaC_objbarrier(ls->L, f, varname);
  return fs->ndebugvars++;
}


/*
** Create a new variable with the given 'name' and given 'kind'.
** Return its index in the function.
*/
static int new_varkind (LexState *ls, TString *name, lu_byte kind) {
  lua_State *L = ls->L;
  FuncState *fs = ls->fs;
  Dyndata *dyd = ls->dyd;
  Vardesc *var;
  luaM_growvector(L, dyd->actvar.arr, dyd->actvar.n + 1,
             dyd->actvar.size, Vardesc, SHRT_MAX, "variable declarations");
  var = &dyd->actvar.arr[dyd->actvar.n++];
  var->vd.kind = kind;  /* default */
  var->vd.name = name;
  var->vd.type_annotation = NULL;
  return dyd->actvar.n - 1 - fs->firstlocal;
}


/*
** Create a new local variable with the given 'name' and regular kind.
*/
static int new_localvar (LexState *ls, TString *name) {
  return new_varkind(ls, name, VDKREG);
}

#define new_localvarliteral(ls,v) \
    new_localvar(ls,  \
      luaX_newstring(ls, "" v, (sizeof(v)/sizeof(char)) - 1));



/*
** Return the "variable description" (Vardesc) of a given variable.
** (Unless noted otherwise, all variables are referred to by their
** compiler indices.)
*/
static Vardesc *getlocalvardesc (FuncState *fs, int vidx) {
  return &fs->ls->dyd->actvar.arr[fs->firstlocal + vidx];
}


/*
** Convert 'nvar', a compiler index level, to its corresponding
** register. For that, search for the highest variable below that level
** that is in a register and uses its register index ('ridx') plus one.
*/
static lu_byte reglevel (FuncState *fs, int nvar) {
  while (nvar-- > 0) {
    Vardesc *vd = getlocalvardesc(fs, nvar);  /* get previous variable */
    if (varinreg(vd))  /* is in a register? */
      return cast_byte(vd->vd.ridx + 1);
  }
  return 0;  /* no variables in registers */
}


/*
** Return the number of variables in the register stack for the given
** function.
*/
lu_byte luaY_nvarstack (FuncState *fs) {
  return reglevel(fs, fs->nactvar);
}


/*
** Get the debug-information entry for current variable 'vidx'.
*/
static LocVar *localdebuginfo (FuncState *fs, int vidx) {
  Vardesc *vd = getlocalvardesc(fs,  vidx);
  if (!varinreg(vd))
    return NULL;  /* no debug info. for constants */
  else {
    int idx = vd->vd.pidx;
    lua_assert(idx < fs->ndebugvars);
    return &fs->f->locvars[idx];
  }
}


/*
** Create an expression representing variable 'vidx'
*/
static void init_var (FuncState *fs, expdesc *e, int vidx) {
  e->f = e->t = NO_JUMP;
  e->k = VLOCAL;
  e->u.var.vidx = cast_short(vidx);
  e->u.var.ridx = getlocalvardesc(fs, vidx)->vd.ridx;
}


/*
** Raises an error if variable described by 'e' is read only; moreover,
** if 'e' is t[exp] where t is the vararg parameter, change it to index
** a real table. (Virtual vararg tables cannot be changed.)
*/
static void check_readonly (LexState *ls, expdesc *e) {
  FuncState *fs = ls->fs;
  TString *varname = NULL;  /* to be set if variable is const */
  switch (e->k) {
    case VCONST: {
      varname = ls->dyd->actvar.arr[e->u.info].vd.name;
      break;
    }
    case VLOCAL: case VVARGVAR: {
      Vardesc *vardesc = getlocalvardesc(fs, e->u.var.vidx);
      if (vardesc->vd.kind != VDKREG)  /* not a regular variable? */
        varname = vardesc->vd.name;
      break;
    }
    case VUPVAL: {
      Upvaldesc *up = &fs->f->upvalues[e->u.info];
      if (up->kind != VDKREG)
        varname = up->name;
      break;
    }
    case VVARGIND: {
      needvatab(fs->f);  /* function will need a vararg table */
      e->k = VINDEXED;
    }  /* FALLTHROUGH */
    case VINDEXUP: case VINDEXSTR: case VINDEXED: {  /* global variable */
      if (e->u.ind.ro)  /* read-only? */
        varname = tsvalue(&fs->f->k[e->u.ind.keystr]);
      break;
    }
    default:
      lua_assert(e->k == VINDEXI);  /* this one doesn't need any check */
      return;  /* integer index cannot be read-only */
  }
  if (varname)
    luaK_semerror(ls, "attempt to assign to const variable '%s'",
                      getstr(varname));
}


/*
** Start the scope for the last 'nvars' created variables.
*/
static void adjustlocalvars (LexState *ls, int nvars) {
  FuncState *fs = ls->fs;
  int reglevel = luaY_nvarstack(fs);
  int i;
  for (i = 0; i < nvars; i++) {
    int vidx = fs->nactvar++;
    Vardesc *var = getlocalvardesc(fs, vidx);
    var->vd.ridx = cast_byte(reglevel++);
    var->vd.pidx = registerlocalvar(ls, fs, var->vd.name);
    /* transfer type annotation to debug info */
    if (var->vd.type_annotation != NULL) {
      fs->f->locvars[var->vd.pidx].typename_ = var->vd.type_annotation;
      luaC_objbarrier(ls->L, fs->f, var->vd.type_annotation);
    }
    luaY_checklimit(fs, reglevel, MAXVARS, "local variables");
  }
}


/*
** Close the scope for all variables up to level 'tolevel'.
** (debug info.)
*/
static void removevars (FuncState *fs, int tolevel) {
  fs->ls->dyd->actvar.n -= (fs->nactvar - tolevel);
  while (fs->nactvar > tolevel) {
    LocVar *var = localdebuginfo(fs, --fs->nactvar);
    if (var)  /* does it have debug information? */
      var->endpc = fs->pc;
  }
}


/*
** Search the upvalues of the function 'fs' for one
** with the given 'name'.
*/
static int searchupvalue (FuncState *fs, TString *name) {
  int i;
  Upvaldesc *up = fs->f->upvalues;
  for (i = 0; i < fs->nups; i++) {
    if (eqstr(up[i].name, name)) return i;
  }
  return -1;  /* not found */
}


static Upvaldesc *allocupvalue (FuncState *fs) {
  Proto *f = fs->f;
  int oldsize = f->sizeupvalues;
  luaY_checklimit(fs, fs->nups + 1, MAXUPVAL, "upvalues");
  luaM_growvector(fs->ls->L, f->upvalues, fs->nups, f->sizeupvalues,
                  Upvaldesc, MAXUPVAL, "upvalues");
  while (oldsize < f->sizeupvalues)
    f->upvalues[oldsize++].name = NULL;
  return &f->upvalues[fs->nups++];
}


static int newupvalue (FuncState *fs, TString *name, expdesc *v) {
  Upvaldesc *up = allocupvalue(fs);
  FuncState *prev = fs->prev;
  if (v->k == VLOCAL) {
    up->instack = 1;
    up->idx = v->u.var.ridx;
    up->kind = getlocalvardesc(prev, v->u.var.vidx)->vd.kind;
    lua_assert(eqstr(name, getlocalvardesc(prev, v->u.var.vidx)->vd.name));
  }
  else {
    up->instack = 0;
    up->idx = cast_byte(v->u.info);
    up->kind = prev->f->upvalues[v->u.info].kind;
    lua_assert(eqstr(name, prev->f->upvalues[v->u.info].name));
  }
  up->name = name;
  luaC_objbarrier(fs->ls->L, fs->f, name);
  return fs->nups - 1;
}


/*
** Look for an active variable with the name 'n' in the
** function 'fs'. If found, initialize 'var' with it and return
** its expression kind; otherwise return -1. While searching,
** var->u.info==-1 means that the preambular global declaration is
** active (the default while there is no other global declaration);
** var->u.info==-2 means there is no active collective declaration
** (some previous global declaration but no collective declaration);
** and var->u.info>=0 points to the inner-most (the first one found)
** collective declaration, if there is one.
*/
static int searchvar (FuncState *fs, TString *n, expdesc *var) {
  int i;
  for (i = cast_int(fs->nactvar) - 1; i >= 0; i--) {
    Vardesc *vd = getlocalvardesc(fs, i);
    if (varglobal(vd)) {  /* global declaration? */
      if (vd->vd.name == NULL) {  /* collective declaration? */
        if (var->u.info < 0)  /* no previous collective declaration? */
          var->u.info = fs->firstlocal + i;  /* this is the first one */
      }
      else {  /* global name */
        if (eqstr(n, vd->vd.name)) {  /* found? */
          init_exp(var, VGLOBAL, fs->firstlocal + i);
          return VGLOBAL;
        }
        else if (var->u.info == -1)  /* active preambular declaration? */
          var->u.info = -2;  /* invalidate preambular declaration */
      }
    }
    else if (eqstr(n, vd->vd.name)) {  /* found? */
      if (vd->vd.kind == RDKCTC)  /* compile-time constant? */
        init_exp(var, VCONST, fs->firstlocal + i);
      else {  /* local variable */
        init_var(fs, var, i);
        if (vd->vd.kind == RDKVAVAR)  /* vararg parameter? */
          var->k = VVARGVAR;
      }
      return cast_int(var->k);
    }
  }
  return -1;  /* not found */
}


/*
** Mark block where variable at given level was defined
** (to emit close instructions later).
*/
static void markupval (FuncState *fs, int level) {
  BlockCnt *bl = fs->bl;
  while (bl->nactvar > level)
    bl = bl->previous;
  bl->upval = 1;
  fs->needclose = 1;
}


/*
** Mark that current block has a to-be-closed variable.
*/
static void marktobeclosed (FuncState *fs) {
  BlockCnt *bl = fs->bl;
  bl->upval = 1;
  bl->insidetbc = 1;
  fs->needclose = 1;
}


/*
** Find a variable with the given name 'n'. If it is an upvalue, add
** this upvalue into all intermediate functions. If it is a global, set
** 'var' as 'void' as a flag.
*/
static void singlevaraux (FuncState *fs, TString *n, expdesc *var, int base) {
  int v = searchvar(fs, n, var);  /* look up variables at current level */
  if (v >= 0) {  /* found? */
    if (!base) {
      if (var->k == VVARGVAR)  /* vararg parameter? */
        luaK_vapar2local(fs, var);  /* change it to a regular local */
      if (var->k == VLOCAL)
        markupval(fs, var->u.var.vidx);  /* will be used as an upvalue */
    }
    /* else nothing else to be done */
  }
  else {  /* not found at current level; try upvalues */
    int idx = searchupvalue(fs, n);  /* try existing upvalues */
    if (idx < 0) {  /* not found? */
      if (fs->prev != NULL)  /* more levels? */
        singlevaraux(fs->prev, n, var, 0);  /* try upper levels */
      if (var->k == VLOCAL || var->k == VUPVAL)  /* local or upvalue? */
        idx  = newupvalue(fs, n, var);  /* will be a new upvalue */
      else  /* it is a global or a constant */
        return;  /* don't need to do anything at this level */
    }
    init_exp(var, VUPVAL, idx);  /* new or old upvalue */
  }
}


static void buildglobal (LexState *ls, TString *varname, expdesc *var) {
  FuncState *fs = ls->fs;
  expdesc key;
  init_exp(var, VGLOBAL, -1);  /* global by default */
  singlevaraux(fs, ls->envn, var, 1);  /* get environment variable */
  if (var->k == VGLOBAL)
    luaK_semerror(ls, "%s is global when accessing variable '%s'",
                      LUA_ENV, getstr(varname));
  luaK_exp2anyregup(fs, var);  /* _ENV could be a constant */
  codestring(&key, varname);  /* key is variable name */
  luaK_indexed(fs, var, &key);  /* 'var' represents _ENV[varname] */
}


/*
** Find a variable with the given name 'n', handling global variables
** too.
*/
static void buildvar (LexState *ls, TString *varname, expdesc *var) {
  FuncState *fs = ls->fs;
  /* 'super' keyword: resolve to parent class in class method context */
  if (fs->classctx != NULL &&
      strcmp(getstr(varname), "super") == 0) {
    /* find parent of current class */
    for (int i = 0; i < ls->nclassparents; i++) {
      if (ls->classparents[i].classname == fs->classctx &&
          ls->classparents[i].parentname != NULL) {
        varname = ls->classparents[i].parentname;
        break;
      }
    }
  }
  init_exp(var, VGLOBAL, -1);  /* global by default */
  singlevaraux(fs, varname, var, 1);
  if (var->k == VGLOBAL) {  /* global name? */
    int info = var->u.info;
    /* global by default in the scope of a global declaration? */
    if (info == -2)
      luaK_semerror(ls, "variable '%s' not declared", getstr(varname));
    buildglobal(ls, varname, var);
    if (info != -1 && ls->dyd->actvar.arr[info].vd.kind == GDKCONST)
      var->u.ind.ro = 1;  /* mark variable as read-only */
    else  /* anyway must be a global */
      lua_assert(info == -1 || ls->dyd->actvar.arr[info].vd.kind == GDKREG);
  }
}


static void singlevar (LexState *ls, expdesc *var) {
  buildvar(ls, str_checkname(ls), var);
}


/*
** Adjust the number of results from an expression list 'e' with 'nexps'
** expressions to 'nvars' values.
*/
static void adjust_assign (LexState *ls, int nvars, int nexps, expdesc *e) {
  FuncState *fs = ls->fs;
  int needed = nvars - nexps;  /* extra values needed */
  luaK_checkstack(fs, needed);
  if (hasmultret(e->k)) {  /* last expression has multiple returns? */
    int extra = needed + 1;  /* discount last expression itself */
    if (extra < 0)
      extra = 0;
    luaK_setreturns(fs, e, extra);  /* last exp. provides the difference */
  }
  else {
    if (e->k != VVOID)  /* at least one expression? */
      luaK_exp2nextreg(fs, e);  /* close last expression */
    if (needed > 0)  /* missing values? */
      luaK_nil(fs, fs->freereg, needed);  /* complete with nils */
  }
  if (needed > 0)
    luaK_reserveregs(fs, needed);  /* registers for extra values */
  else  /* adding 'needed' is actually a subtraction */
    fs->freereg = cast_byte(fs->freereg + needed);  /* remove extra values */
}


#define enterlevel(ls)	luaE_incCstack(ls->L)


#define leavelevel(ls) ((ls)->L->nCcalls--)


/*
** Generates an error that a goto jumps into the scope of some
** variable declaration.
*/
static l_noret jumpscopeerror (LexState *ls, Labeldesc *gt) {
  TString *tsname = getlocalvardesc(ls->fs, gt->nactvar)->vd.name;
  const char *varname = (tsname != NULL) ? getstr(tsname) : "*";
  luaK_semerror(ls,
     "<goto %s> at line %d jumps into the scope of '%s'",
      getstr(gt->name), gt->line, varname);  /* raise the error */
}


/*
** Closes the goto at index 'g' to given 'label' and removes it
** from the list of pending gotos.
** If it jumps into the scope of some variable, raises an error.
** The goto needs a CLOSE if it jumps out of a block with upvalues,
** or out of the scope of some variable and the block has upvalues
** (signaled by parameter 'bup').
*/
static void closegoto (LexState *ls, int g, Labeldesc *label, int bup) {
  int i;
  FuncState *fs = ls->fs;
  Labellist *gl = &ls->dyd->gt;  /* list of gotos */
  Labeldesc *gt = &gl->arr[g];  /* goto to be resolved */
  lua_assert(eqstr(gt->name, label->name));
  if (l_unlikely(gt->nactvar < label->nactvar))  /* enter some scope? */
    jumpscopeerror(ls, gt);
  if (gt->close ||
      (label->nactvar < gt->nactvar && bup)) {  /* needs close? */
    lu_byte stklevel = reglevel(fs, label->nactvar);
    /* move jump to CLOSE position */
    fs->f->code[gt->pc + 1] = fs->f->code[gt->pc];
    /* put CLOSE instruction at original position */
    fs->f->code[gt->pc] = CREATE_ABCk(OP_CLOSE, stklevel, 0, 0, 0);
    gt->pc++;  /* must point to jump instruction */
  }
  luaK_patchlist(ls->fs, gt->pc, label->pc);  /* goto jumps to label */
  for (i = g; i < gl->n - 1; i++)  /* remove goto from pending list */
    gl->arr[i] = gl->arr[i + 1];
  gl->n--;
}


/*
** Search for an active label with the given name, starting at
** index 'ilb' (so that it can search for all labels in current block
** or all labels in current function).
*/
static Labeldesc *findlabel (LexState *ls, TString *name, int ilb) {
  Dyndata *dyd = ls->dyd;
  for (; ilb < dyd->label.n; ilb++) {
    Labeldesc *lb = &dyd->label.arr[ilb];
    if (eqstr(lb->name, name))  /* correct label? */
      return lb;
  }
  return NULL;  /* label not found */
}


/*
** Adds a new label/goto in the corresponding list.
*/
static int newlabelentry (LexState *ls, Labellist *l, TString *name,
                          int line, int pc) {
  int n = l->n;
  luaM_growvector(ls->L, l->arr, n, l->size,
                  Labeldesc, SHRT_MAX, "labels/gotos");
  l->arr[n].name = name;
  l->arr[n].line = line;
  l->arr[n].nactvar = ls->fs->nactvar;
  l->arr[n].close = 0;
  l->arr[n].pc = pc;
  l->n = n + 1;
  return n;
}


/*
** Create an entry for the goto and the code for it. As it is not known
** at this point whether the goto may need a CLOSE, the code has a jump
** followed by an CLOSE. (As the CLOSE comes after the jump, it is a
** dead instruction; it works as a placeholder.) When the goto is closed
** against a label, if it needs a CLOSE, the two instructions swap
** positions, so that the CLOSE comes before the jump.
*/
static int newgotoentry (LexState *ls, TString *name, int line) {
  FuncState *fs = ls->fs;
  int pc = luaK_jump(fs);  /* create jump */
  luaK_codeABC(fs, OP_CLOSE, 0, 1, 0);  /* spaceholder, marked as dead */
  return newlabelentry(ls, &ls->dyd->gt, name, line, pc);
}


/*
** Create a new label with the given 'name' at the given 'line'.
** 'last' tells whether label is the last non-op statement in its
** block. Solves all pending gotos to this new label and adds
** a close instruction if necessary.
** Returns true iff it added a close instruction.
*/
static void createlabel (LexState *ls, TString *name, int line, int last) {
  FuncState *fs = ls->fs;
  Labellist *ll = &ls->dyd->label;
  int l = newlabelentry(ls, ll, name, line, luaK_getlabel(fs));
  if (last) {  /* label is last no-op statement in the block? */
    /* assume that locals are already out of scope */
    ll->arr[l].nactvar = fs->bl->nactvar;
  }
}


/*
** Traverse the pending gotos of the finishing block checking whether
** each match some label of that block. Those that do not match are
** "exported" to the outer block, to be solved there. In particular,
** its 'nactvar' is updated with the level of the inner block,
** as the variables of the inner block are now out of scope.
*/
static void solvegotos (FuncState *fs, BlockCnt *bl) {
  LexState *ls = fs->ls;
  Labellist *gl = &ls->dyd->gt;
  int outlevel = reglevel(fs, bl->nactvar);  /* level outside the block */
  int igt = bl->firstgoto;  /* first goto in the finishing block */
  while (igt < gl->n) {   /* for each pending goto */
    Labeldesc *gt = &gl->arr[igt];
    /* search for a matching label in the current block */
    Labeldesc *lb = findlabel(ls, gt->name, bl->firstlabel);
    if (lb != NULL)  /* found a match? */
      closegoto(ls, igt, lb, bl->upval);  /* close and remove goto */
    else {  /* adjust 'goto' for outer block */
      /* block has variables to be closed and goto escapes the scope of
         some variable? */
      if (bl->upval && reglevel(fs, gt->nactvar) > outlevel)
        gt->close = 1;  /* jump may need a close */
      gt->nactvar = bl->nactvar;  /* correct level for outer block */
      igt++;  /* go to next goto */
    }
  }
  ls->dyd->label.n = bl->firstlabel;  /* remove local labels */
}


static void enterblock (FuncState *fs, BlockCnt *bl, lu_byte isloop) {
  bl->isloop = isloop;
  bl->nactvar = fs->nactvar;
  bl->firstlabel = fs->ls->dyd->label.n;
  bl->firstgoto = fs->ls->dyd->gt.n;
  bl->upval = 0;
  /* inherit 'insidetbc' from enclosing block */
  bl->insidetbc = (fs->bl != NULL && fs->bl->insidetbc);
  bl->previous = fs->bl;  /* link block in function's block list */
  fs->bl = bl;
  lua_assert(fs->freereg == luaY_nvarstack(fs));
}


/*
** generates an error for an undefined 'goto'.
*/
static l_noret undefgoto (LexState *ls, Labeldesc *gt) {
  /* breaks are checked when created, cannot be undefined */
  lua_assert(!eqstr(gt->name, ls->brkn));
  luaK_semerror(ls, "no visible label '%s' for <goto> at line %d",
                    getstr(gt->name), gt->line);
}


static void leaveblock (FuncState *fs) {
  BlockCnt *bl = fs->bl;
  LexState *ls = fs->ls;
  lu_byte stklevel = reglevel(fs, bl->nactvar);  /* level outside block */
  if (bl->previous && bl->upval)  /* need a 'close'? */
    luaK_codeABC(fs, OP_CLOSE, stklevel, 0, 0);
  fs->freereg = stklevel;  /* free registers */
  removevars(fs, bl->nactvar);  /* remove block locals */
  lua_assert(bl->nactvar == fs->nactvar);  /* back to level on entry */
  if (bl->isloop == 2)  /* has to fix pending breaks? */
    createlabel(ls, ls->brkn, 0, 0);
  solvegotos(fs, bl);
  if (bl->previous == NULL) {  /* was it the last block? */
    if (bl->firstgoto < ls->dyd->gt.n)  /* still pending gotos? */
      undefgoto(ls, &ls->dyd->gt.arr[bl->firstgoto]);  /* error */
  }
  fs->bl = bl->previous;  /* current block now is previous one */
}


/*
** adds a new prototype into list of prototypes
*/
static Proto *addprototype (LexState *ls) {
  Proto *clp;
  lua_State *L = ls->L;
  FuncState *fs = ls->fs;
  Proto *f = fs->f;  /* prototype of current function */
  if (fs->np >= f->sizep) {
    int oldsize = f->sizep;
    luaM_growvector(L, f->p, fs->np, f->sizep, Proto *, MAXARG_Bx, "functions");
    while (oldsize < f->sizep)
      f->p[oldsize++] = NULL;
  }
  f->p[fs->np++] = clp = luaF_newproto(L);
  luaC_objbarrier(L, f, clp);
  return clp;
}


/*
** codes instruction to create new closure in parent function.
** The OP_CLOSURE instruction uses the last available register,
** so that, if it invokes the GC, the GC knows which registers
** are in use at that time.

*/
static void codeclosure (LexState *ls, expdesc *v) {
  FuncState *fs = ls->fs->prev;
  init_exp(v, VRELOC, luaK_codeABx(fs, OP_CLOSURE, 0, fs->np - 1));
  luaK_exp2nextreg(fs, v);  /* fix it at the last register */
}


static void open_func (LexState *ls, FuncState *fs, BlockCnt *bl) {
  lua_State *L = ls->L;
  Proto *f = fs->f;
  fs->prev = ls->fs;  /* linked list of funcstates */
  fs->ls = ls;
  ls->fs = fs;
  fs->pc = 0;
  fs->previousline = f->linedefined;
  fs->iwthabs = 0;
  fs->lasttarget = 0;
  fs->freereg = 0;
  fs->nk = 0;
  fs->nabslineinfo = 0;
  fs->np = 0;
  fs->nups = 0;
  fs->ndebugvars = 0;
  fs->nactvar = 0;
  fs->needclose = 0;
  fs->classctx = NULL;  /* not inside a class method by default */
  fs->firstlocal = ls->dyd->actvar.n;
  fs->firstlabel = ls->dyd->label.n;
  fs->bl = NULL;
  f->source = ls->source;
  luaC_objbarrier(L, f, f->source);
  f->maxstacksize = 2;  /* registers 0/1 are always valid */
  fs->kcache = luaH_new(L);  /* create table for function */
  sethvalue2s(L, L->top.p, fs->kcache);  /* anchor it */
  luaD_inctop(L);
  enterblock(fs, bl, 0);
}


static void close_func (LexState *ls) {
  lua_State *L = ls->L;
  FuncState *fs = ls->fs;
  Proto *f = fs->f;
  luaK_ret(fs, luaY_nvarstack(fs), 0);  /* final return */
  leaveblock(fs);
  lua_assert(fs->bl == NULL);
  luaK_finish(fs);
  luaM_shrinkvector(L, f->code, f->sizecode, fs->pc, Instruction);
  luaM_shrinkvector(L, f->lineinfo, f->sizelineinfo, fs->pc, ls_byte);
  luaM_shrinkvector(L, f->abslineinfo, f->sizeabslineinfo,
                       fs->nabslineinfo, AbsLineInfo);
  luaM_shrinkvector(L, f->k, f->sizek, fs->nk, TValue);
  luaM_shrinkvector(L, f->p, f->sizep, fs->np, Proto *);
  luaM_shrinkvector(L, f->locvars, f->sizelocvars, fs->ndebugvars, LocVar);
  luaM_shrinkvector(L, f->upvalues, f->sizeupvalues, fs->nups, Upvaldesc);
  ls->fs = fs->prev;
  L->top.p--;  /* pop kcache table */
  luaC_checkGC(L);
}


/*
** {======================================================================
** GRAMMAR RULES
** =======================================================================
*/


/*
** check whether current token is in the follow set of a block.
** 'until' closes syntactical blocks, but do not close scope,
** so it is handled in separate.
*/
static int block_follow (LexState *ls, int withuntil) {
  switch (ls->t.token) {
    case TK_ELSE: case TK_ELSEIF:
    case TK_END: case TK_EOS:
      return 1;
    case TK_UNTIL: return withuntil;
    default: return 0;
  }
}


static void statlist (LexState *ls) {
  /* statlist -> { stat [';'] } */
  while (!block_follow(ls, 1)) {
    if (ls->t.token == TK_RETURN) {
      statement(ls);
      return;  /* 'return' must be last statement */
    }
    statement(ls);
  }
}


static void fieldsel (LexState *ls, expdesc *v) {
  /* fieldsel -> ['.' | ':'] NAME */
  FuncState *fs = ls->fs;
  expdesc key;
  /* compile-time access check: if v is a typed local variable with a class
     type, check if the field is private/protected */
  if (v->k == VLOCAL && ls->nclassfields > 0) {
    Vardesc *vd = getlocalvardesc(fs, v->u.var.vidx);
    if (vd->vd.type_annotation != NULL) {
      /* peek at the field name (next token after dot/colon) */
      int nexttoken = luaX_lookahead(ls);
      if (nexttoken == TK_NAME) {
        TString *fieldname = ls->lookahead.seminfo.ts;
        int blocked = check_field_access(ls, vd->vd.type_annotation, fieldname);
        if (blocked == ACCESS_PRIVATE)
          luaK_semerror(ls,
            "cannot access private field '%s' of class '%s'",
            getstr(fieldname), getstr(vd->vd.type_annotation));
        else if (blocked == ACCESS_PROTECTED)
          luaK_semerror(ls,
            "cannot access protected field '%s' of class '%s'",
            getstr(fieldname), getstr(vd->vd.type_annotation));
      }
    }
  }
  luaK_exp2anyregup(fs, v);
  luaX_next(ls);  /* skip the dot or colon */
  codename(ls, &key);
  luaK_indexed(fs, v, &key);
}


static void yindex (LexState *ls, expdesc *v) {
  /* index -> '[' expr ']' */
  luaX_next(ls);  /* skip the '[' */
  expr(ls, v);
  luaK_exp2val(ls->fs, v);
  checknext(ls, ']');
}


/*
** {======================================================================
** Rules for Constructors
** =======================================================================
*/

typedef struct ConsControl {
  expdesc v;  /* last list item read */
  expdesc *t;  /* table descriptor */
  int nh;  /* total number of 'record' elements */
  int na;  /* number of array elements already stored */
  int tostore;  /* number of array elements pending to be stored */
  int maxtostore;  /* maximum number of pending elements */
} ConsControl;


/*
** Maximum number of elements in a constructor, to control the following:
** * counter overflows;
** * overflows in 'extra' for OP_NEWTABLE and OP_SETLIST;
** * overflows when adding multiple returns in OP_SETLIST.
*/
#define MAX_CNST	(INT_MAX/2)
#if MAX_CNST/(MAXARG_vC + 1) > MAXARG_Ax
#undef MAX_CNST
#define MAX_CNST	(MAXARG_Ax * (MAXARG_vC + 1))
#endif


static void recfield (LexState *ls, ConsControl *cc) {
  /* recfield -> (NAME | '['exp']') = exp */
  FuncState *fs = ls->fs;
  lu_byte reg = ls->fs->freereg;
  expdesc tab, key, val;
  if (ls->t.token == TK_NAME)
    codename(ls, &key);
  else  /* ls->t.token == '[' */
    yindex(ls, &key);
  cc->nh++;
  checknext(ls, '=');
  tab = *cc->t;
  luaK_indexed(fs, &tab, &key);
  expr(ls, &val);
  luaK_storevar(fs, &tab, &val);
  fs->freereg = reg;  /* free registers */
}


static void closelistfield (FuncState *fs, ConsControl *cc) {
  lua_assert(cc->tostore > 0);
  luaK_exp2nextreg(fs, &cc->v);
  cc->v.k = VVOID;
  if (cc->tostore >= cc->maxtostore) {
    luaK_setlist(fs, cc->t->u.info, cc->na, cc->tostore);  /* flush */
    cc->na += cc->tostore;
    cc->tostore = 0;  /* no more items pending */
  }
}


static void lastlistfield (FuncState *fs, ConsControl *cc) {
  if (cc->tostore == 0) return;
  if (hasmultret(cc->v.k)) {
    luaK_setmultret(fs, &cc->v);
    luaK_setlist(fs, cc->t->u.info, cc->na, LUA_MULTRET);
    cc->na--;  /* do not count last expression (unknown number of elements) */
  }
  else {
    if (cc->v.k != VVOID)
      luaK_exp2nextreg(fs, &cc->v);
    luaK_setlist(fs, cc->t->u.info, cc->na, cc->tostore);
  }
  cc->na += cc->tostore;
}


static void listfield (LexState *ls, ConsControl *cc) {
  /* listfield -> exp */
  expr(ls, &cc->v);
  cc->tostore++;
}


static void field (LexState *ls, ConsControl *cc) {
  /* field -> listfield | recfield */
  switch(ls->t.token) {
    case TK_NAME: {  /* may be 'listfield' or 'recfield' */
      if (luaX_lookahead(ls) != '=')  /* expression? */
        listfield(ls, cc);
      else
        recfield(ls, cc);
      break;
    }
    case '[': {
      recfield(ls, cc);
      break;
    }
    default: {
      listfield(ls, cc);
      break;
    }
  }
}


/*
** Compute a limit for how many registers a constructor can use before
** emitting a 'SETLIST' instruction, based on how many registers are
** available.
*/
static int maxtostore (FuncState *fs) {
  int numfreeregs = MAX_FSTACK - fs->freereg;
  if (numfreeregs >= 160)  /* "lots" of registers? */
    return numfreeregs / 5;  /* use up to 1/5 of them */
  else if (numfreeregs >= 80)  /* still "enough" registers? */
    return 10;  /* one 'SETLIST' instruction for each 10 values */
  else  /* save registers for potential more nesting */
    return 1;
}


static void constructor (LexState *ls, expdesc *t) {
  /* constructor -> '{' [ field { sep field } [sep] ] '}'
     sep -> ',' | ';' */
  FuncState *fs = ls->fs;
  int line = ls->linenumber;
  int pc = luaK_codevABCk(fs, OP_NEWTABLE, 0, 0, 0, 0);
  ConsControl cc;
  luaK_code(fs, 0);  /* space for extra arg. */
  cc.na = cc.nh = cc.tostore = 0;
  cc.t = t;
  init_exp(t, VNONRELOC, fs->freereg);  /* table will be at stack top */
  luaK_reserveregs(fs, 1);
  init_exp(&cc.v, VVOID, 0);  /* no value (yet) */
  checknext(ls, '{' /*}*/);
  cc.maxtostore = maxtostore(fs);
  do {
    if (ls->t.token == /*{*/ '}') break;
    if (cc.v.k != VVOID)  /* is there a previous list item? */
      closelistfield(fs, &cc);  /* close it */
    field(ls, &cc);
    luaY_checklimit(fs, cc.tostore + cc.na + cc.nh, MAX_CNST,
                    "items in a constructor");
  } while (testnext(ls, ',') || testnext(ls, ';'));
  check_match(ls, /*{*/ '}', '{' /*}*/, line);
  lastlistfield(fs, &cc);
  luaK_settablesize(fs, pc, t->u.info, cc.na, cc.nh);
}

/* }====================================================================== */


static void setvararg (FuncState *fs) {
  fs->f->flag |= PF_VAHID;  /* by default, use hidden vararg arguments */
  luaK_codeABC(fs, OP_VARARGPREP, 0, 0, 0);
}


static void parlist (LexState *ls) {
  /* parlist -> [ {NAME [':' type] ','} (NAME [':' type] | '...') ] */
  FuncState *fs = ls->fs;
  Proto *f = fs->f;
  int nparams = 0;
  int varargk = 0;
  if (ls->t.token != ')') {  /* is 'parlist' not empty? */
    do {
      switch (ls->t.token) {
        case TK_NAME: {
          new_localvar(ls, str_checkname(ls));
          optional_type_annotation(ls);  /* skip optional ': type' */
          nparams++;
          break;
        }
        case TK_DOTS: {
          varargk = 1;
          luaX_next(ls);  /* skip '...' */
          if (ls->t.token == TK_NAME)
            new_varkind(ls, str_checkname(ls), RDKVAVAR);
          else
            new_localvarliteral(ls, "(vararg table)");
          break;
        }
        default: luaX_syntaxerror(ls, "<name> or '...' expected");
      }
    } while (!varargk && testnext(ls, ','));
  }
  adjustlocalvars(ls, nparams);
  f->numparams = cast_byte(fs->nactvar);
  if (varargk) {
    setvararg(fs);  /* declared vararg */
    adjustlocalvars(ls, 1);  /* vararg parameter */
  }
  /* reserve registers for parameters (plus vararg parameter, if present) */
  luaK_reserveregs(fs, fs->nactvar);
}


static void body_classctx (LexState *ls, expdesc *e, int ismethod,
                           int line, TString *classctx);

static void body (LexState *ls, expdesc *e, int ismethod, int line) {
  body_classctx(ls, e, ismethod, line, NULL);
}

static void body_classctx (LexState *ls, expdesc *e, int ismethod,
                           int line, TString *classctx) {
  /* body ->  '(' parlist ')' [':' type] block END */
  FuncState new_fs;
  BlockCnt bl;
  new_fs.f = addprototype(ls);
  new_fs.f->linedefined = line;
  open_func(ls, &new_fs, &bl);
  new_fs.classctx = classctx;  /* set class context for access checking */
  checknext(ls, '(');
  if (ismethod) {
    new_localvarliteral(ls, "self");  /* create 'self' parameter */
    adjustlocalvars(ls, 1);
  }
  parlist(ls);
  checknext(ls, ')');
  optional_type_annotation(ls);  /* skip optional return type */
  statlist(ls);
  new_fs.f->lastlinedefined = ls->linenumber;
  check_match(ls, TK_END, TK_FUNCTION, line);
  codeclosure(ls, e);
  close_func(ls);
}


static int explist (LexState *ls, expdesc *v) {
  /* explist -> expr { ',' expr } */
  int n = 1;  /* at least one expression */
  expr(ls, v);
  while (testnext(ls, ',')) {
    luaK_exp2nextreg(ls->fs, v);
    expr(ls, v);
    n++;
  }
  return n;
}


static void funcargs (LexState *ls, expdesc *f) {
  FuncState *fs = ls->fs;
  expdesc args;
  int base, nparams;
  int line = ls->linenumber;
  switch (ls->t.token) {
    case '(': {  /* funcargs -> '(' [ explist ] ')' */
      luaX_next(ls);
      if (ls->t.token == ')')  /* arg list is empty? */
        args.k = VVOID;
      else {
        explist(ls, &args);
        if (hasmultret(args.k))
          luaK_setmultret(fs, &args);
      }
      check_match(ls, ')', '(', line);
      break;
    }
    case '{' /*}*/: {  /* funcargs -> constructor */
      constructor(ls, &args);
      break;
    }
    case TK_STRING: {  /* funcargs -> STRING */
      codestring(&args, ls->t.seminfo.ts);
      luaX_next(ls);  /* must use 'seminfo' before 'next' */
      break;
    }
    default: {
      luaX_syntaxerror(ls, "function arguments expected");
    }
  }
  lua_assert(f->k == VNONRELOC);
  base = f->u.info;  /* base register for call */
  if (hasmultret(args.k))
    nparams = LUA_MULTRET;  /* open call */
  else {
    if (args.k != VVOID)
      luaK_exp2nextreg(fs, &args);  /* close last argument */
    nparams = fs->freereg - (base+1);
  }
  init_exp(f, VCALL, luaK_codeABC(fs, OP_CALL, base, nparams+1, 2));
  luaK_fixline(fs, line);
  /* call removes function and arguments and leaves one result (unless
     changed later) */
  fs->freereg = cast_byte(base + 1);
}




/*
** {======================================================================
** Expression parsing
** =======================================================================
*/


static void primaryexp (LexState *ls, expdesc *v) {
  /* primaryexp -> NAME | '(' expr ')' */
  switch (ls->t.token) {
    case '(': {
      int line = ls->linenumber;
      luaX_next(ls);
      expr(ls, v);
      check_match(ls, ')', '(', line);
      luaK_dischargevars(ls->fs, v);
      return;
    }
    case TK_NAME: {
      singlevar(ls, v);
      return;
    }
    default: {
      luaX_syntaxerror(ls, "unexpected symbol");
    }
  }
}


static void suffixedexp (LexState *ls, expdesc *v) {
  /* suffixedexp ->
       primaryexp { '.' NAME | '[' exp ']' | ':' NAME funcargs | funcargs } */
  FuncState *fs = ls->fs;
  primaryexp(ls, v);
  for (;;) {
    switch (ls->t.token) {
      case '.': {  /* fieldsel */
        fieldsel(ls, v);
        break;
      }
      case '?': {  /* nullable chaining: ?. or ?[ */
        luaX_next(ls);  /* skip '?' */
        if (ls->t.token == '.' || ls->t.token == '[') {
          /* a?.b compiles to:
               local __tmp = a
               if __tmp == nil then result = nil
               else result = __tmp.b end
          */
          int resultreg;
          luaK_exp2nextreg(fs, v);
          resultreg = fs->freereg - 1;
          /* TEST resultreg k=0: skip JMP if truthy */
          luaK_codeABCk(fs, OP_TEST, resultreg, 0, 0, 0);
          int jmp_nil = luaK_jump(fs);
          /* not nil: do field access (result stays in resultreg) */
          init_exp(v, VNONRELOC, resultreg);
          if (ls->t.token == '.')
            fieldsel(ls, v);
          else {
            expdesc key;
            luaK_exp2anyregup(fs, v);
            yindex(ls, &key);
            luaK_indexed(fs, v, &key);
          }
          luaK_exp2nextreg(fs, v);
          if (v->u.info != resultreg)
            luaK_codeABC(fs, OP_MOVE, resultreg, v->u.info, 0);
          fs->freereg = cast_byte(resultreg + 1);
          int jmp_end = luaK_jump(fs);
          /* nil path: store nil in resultreg */
          luaK_patchtohere(fs, jmp_nil);
          luaK_nil(fs, resultreg, 1);
          luaK_patchtohere(fs, jmp_end);
          init_exp(v, VNONRELOC, resultreg);
        }
        else {
          luaX_syntaxerror(ls, "'.' or '[' expected after '?'");
        }
        break;
      }
      case '[': {  /* '[' exp ']' */
        expdesc key;
        luaK_exp2anyregup(fs, v);
        yindex(ls, &key);
        luaK_indexed(fs, v, &key);
        break;
      }
      case ':': {  /* ':' NAME funcargs */
        expdesc key;
        luaX_next(ls);
        codename(ls, &key);
        luaK_self(fs, v, &key);
        funcargs(ls, v);
        break;
      }
      case '(': case TK_STRING: case '{' /*}*/: {  /* funcargs */
        luaK_exp2nextreg(fs, v);
        funcargs(ls, v);
        break;
      }
      default: return;
    }
  }
}


static void simpleexp (LexState *ls, expdesc *v) {
  /* simpleexp -> FLT | INT | STRING | NIL | TRUE | FALSE | ... |
                  constructor | FUNCTION body | suffixedexp */
  switch (ls->t.token) {
    case TK_FLT: {
      init_exp(v, VKFLT, 0);
      v->u.nval = ls->t.seminfo.r;
      break;
    }
    case TK_INT: {
      init_exp(v, VKINT, 0);
      v->u.ival = ls->t.seminfo.i;
      break;
    }
    case TK_STRING: {
      codestring(v, ls->t.seminfo.ts);
      break;
    }
    case TK_NIL: {
      init_exp(v, VNIL, 0);
      break;
    }
    case TK_TRUE: {
      init_exp(v, VTRUE, 0);
      break;
    }
    case TK_FALSE: {
      init_exp(v, VFALSE, 0);
      break;
    }
    case TK_DOTS: {  /* vararg */
      FuncState *fs = ls->fs;
      check_condition(ls, isvararg(fs->f),
                      "cannot use '...' outside a vararg function");
      init_exp(v, VVARARG, luaK_codeABC(fs, OP_VARARG, 0, fs->f->numparams, 1));
      break;
    }
    case '{' /*}*/: {  /* constructor */
      constructor(ls, v);
      return;
    }
    case TK_FUNCTION: {
      luaX_next(ls);
      body(ls, v, 0, ls->linenumber);
      return;
    }
    case '|': {
      /* lambda: |params| expr
         Compiles to: function(params) return expr end */
      FuncState new_fs;
      BlockCnt bl;
      int line = ls->linenumber;
      new_fs.f = addprototype(ls);
      new_fs.f->linedefined = line;
      open_func(ls, &new_fs, &bl);
      luaX_next(ls);  /* skip '|' */
      /* parse parameter list */
      int nparams = 0;
      if (ls->t.token != '|') {
        do {
          new_localvar(ls, str_checkname(ls));
          /* type annotation in lambda: ': type' but stop before '|' */
          if (ls->t.token == ':') {
            luaX_next(ls);  /* skip ':' */
            /* parse simple type name only (no union with |) */
            if (ls->t.token == TK_NAME || ls->t.token == TK_NIL ||
                ls->t.token == TK_FUNCTION)
              luaX_next(ls);
            if (ls->t.token == '?') luaX_next(ls);
          }
          nparams++;
        } while (testnext(ls, ','));
      }
      if (ls->t.token != '|')
        luaX_syntaxerror(ls, "'|' expected to close lambda parameters");
      luaX_next(ls);  /* skip closing '|' */
      adjustlocalvars(ls, nparams);
      new_fs.f->numparams = cast_byte(new_fs.nactvar);
      luaK_reserveregs(&new_fs, new_fs.nactvar);
      /* parse body expression */
      expdesc e;
      expr(ls, &e);
      /* generate: return expr */
      luaK_exp2nextreg(&new_fs, &e);
      luaK_ret(&new_fs, new_fs.nactvar, 1);
      new_fs.f->lastlinedefined = ls->linenumber;
      codeclosure(ls, v);
      close_func(ls);
      return;
    }
    default: {
      /* String interpolation: f"hello {name} world"
         Only supports simple variable references {name}, not expressions.
         Compiles to: "hello " .. tostring(name) .. " world" */
      if (ls->t.token == TK_NAME
          && ls->t.seminfo.ts == luaX_newstring(ls, "f", 1)
          && luaX_lookahead(ls) == TK_STRING) {
        FuncState *fs = ls->fs;
        luaX_next(ls);  /* skip 'f' (lookahead consumed next token) */
        TString *tmpl = ls->t.seminfo.ts;
        const char *s = getstr(tmpl);
        size_t len = tsslen(tmpl);
        luaX_next(ls);  /* skip the string */
        /* Split template into parts, push all onto stack, then concat */
        int nparts = 0;
        int firstreg = fs->freereg;
        size_t pos = 0;
        while (pos <= len) {
          size_t start = pos;
          while (pos < len && s[pos] != '{') pos++;
          /* literal segment */
          if (pos > start) {
            expdesc lit;
            codestring(&lit, luaX_newstring(ls, s + start, pos - start));
            luaK_exp2nextreg(fs, &lit);
            nparts++;
          }
          if (pos >= len) break;
          if (s[pos] == '{') {
            pos++;
            size_t es = pos;
            while (pos < len && s[pos] != '}') pos++;
            if (pos < len) {
              TString *vn = luaX_newstring(ls, s + es, pos - es);
              expdesc ve;
              buildvar(ls, vn, &ve);
              luaK_exp2nextreg(fs, &ve);
              nparts++;
              pos++;  /* skip '}' */
            }
          }
        }
        if (nparts == 0) {
          codestring(v, luaX_newstring(ls, "", 0));
        }
        else if (nparts == 1) {
          init_exp(v, VNONRELOC, firstreg);
        }
        else {
          /* OP_CONCAT A B: R[A] = R[A] .. ... .. R[A+B-1] */
          luaK_codeABC(fs, OP_CONCAT, firstreg, nparts, 0);
          fs->freereg = cast_byte(firstreg + 1);  /* result in firstreg */
          init_exp(v, VNONRELOC, firstreg);
        }
        return;
      }
      suffixedexp(ls, v);
      return;
    }
  }
  luaX_next(ls);
}


static UnOpr getunopr (int op) {
  switch (op) {
    case TK_NOT: return OPR_NOT;
    case '-': return OPR_MINUS;
    case '~': return OPR_BNOT;
    case '#': return OPR_LEN;
    default: return OPR_NOUNOPR;
  }
}


static BinOpr getbinopr (int op) {
  switch (op) {
    case '+': return OPR_ADD;
    case '-': return OPR_SUB;
    case '*': return OPR_MUL;
    case '%': return OPR_MOD;
    case '^': return OPR_POW;
    case '/': return OPR_DIV;
    case TK_IDIV: return OPR_IDIV;
    case '&': return OPR_BAND;
    case '|': return OPR_BOR;
    case '~': return OPR_BXOR;
    case TK_SHL: return OPR_SHL;
    case TK_SHR: return OPR_SHR;
    case TK_CONCAT: return OPR_CONCAT;
    case TK_NE: return OPR_NE;
    case TK_EQ: return OPR_EQ;
    case '<': return OPR_LT;
    case TK_LE: return OPR_LE;
    case '>': return OPR_GT;
    case TK_GE: return OPR_GE;
    case TK_AND: return OPR_AND;
    case TK_OR: return OPR_OR;
    default: return OPR_NOBINOPR;
  }
}


/*
** Priority table for binary operators.
*/
static const struct {
  lu_byte left;  /* left priority for each binary operator */
  lu_byte right; /* right priority */
} priority[] = {  /* ORDER OPR */
   {10, 10}, {10, 10},           /* '+' '-' */
   {11, 11}, {11, 11},           /* '*' '%' */
   {14, 13},                  /* '^' (right associative) */
   {11, 11}, {11, 11},           /* '/' '//' */
   {6, 6}, {4, 4}, {5, 5},   /* '&' '|' '~' */
   {7, 7}, {7, 7},           /* '<<' '>>' */
   {9, 8},                   /* '..' (right associative) */
   {3, 3}, {3, 3}, {3, 3},   /* ==, <, <= */
   {3, 3}, {3, 3}, {3, 3},   /* ~=, >, >= */
   {2, 2}, {1, 1}            /* and, or */
};

#define UNARY_PRIORITY	12  /* priority for unary operators */


/*
** subexpr -> (simpleexp | unop subexpr) { binop subexpr }
** where 'binop' is any binary operator with a priority higher than 'limit'
*/
static BinOpr subexpr (LexState *ls, expdesc *v, int limit) {
  BinOpr op;
  UnOpr uop;
  enterlevel(ls);
  uop = getunopr(ls->t.token);
  if (uop != OPR_NOUNOPR) {  /* prefix (unary) operator? */
    int line = ls->linenumber;
    luaX_next(ls);  /* skip operator */
    subexpr(ls, v, UNARY_PRIORITY);
    luaK_prefix(ls->fs, uop, v, line);
  }
  else simpleexp(ls, v);
  /* expand while operators have priorities higher than 'limit' */
  op = getbinopr(ls->t.token);
  while (op != OPR_NOBINOPR && priority[op].left > limit) {
    expdesc v2;
    BinOpr nextop;
    int line = ls->linenumber;
    luaX_next(ls);  /* skip operator */
    luaK_infix(ls->fs, op, v);
    /* read sub-expression with higher priority */
    nextop = subexpr(ls, &v2, priority[op].right);
    luaK_posfix(ls->fs, op, v, &v2, line);
    op = nextop;
  }
  leavelevel(ls);
  return op;  /* return first untreated operator */
}


static void expr (LexState *ls, expdesc *v) {
  subexpr(ls, v, 0);
}

/* }==================================================================== */



/*
** {======================================================================
** Rules for Statements
** =======================================================================
*/


static void block (LexState *ls) {
  /* block -> statlist */
  FuncState *fs = ls->fs;
  BlockCnt bl;
  enterblock(fs, &bl, 0);
  statlist(ls);
  leaveblock(fs);
}


/*
** structure to chain all variables in the left-hand side of an
** assignment
*/
struct LHS_assign {
  struct LHS_assign *prev;
  expdesc v;  /* variable (global, local, upvalue, or indexed) */
};


/*
** check whether, in an assignment to an upvalue/local variable, the
** upvalue/local variable is begin used in a previous assignment to a
** table. If so, save original upvalue/local value in a safe place and
** use this safe copy in the previous assignment.
*/
static void check_conflict (LexState *ls, struct LHS_assign *lh, expdesc *v) {
  FuncState *fs = ls->fs;
  lu_byte extra = fs->freereg;  /* eventual position to save local variable */
  int conflict = 0;
  for (; lh; lh = lh->prev) {  /* check all previous assignments */
    if (vkisindexed(lh->v.k)) {  /* assignment to table field? */
      if (lh->v.k == VINDEXUP) {  /* is table an upvalue? */
        if (v->k == VUPVAL && lh->v.u.ind.t == v->u.info) {
          conflict = 1;  /* table is the upvalue being assigned now */
          lh->v.k = VINDEXSTR;
          lh->v.u.ind.t = extra;  /* assignment will use safe copy */
        }
      }
      else {  /* table is a register */
        if (v->k == VLOCAL && lh->v.u.ind.t == v->u.var.ridx) {
          conflict = 1;  /* table is the local being assigned now */
          lh->v.u.ind.t = extra;  /* assignment will use safe copy */
        }
        /* is index the local being assigned? */
        if (lh->v.k == VINDEXED && v->k == VLOCAL &&
            lh->v.u.ind.idx == v->u.var.ridx) {
          conflict = 1;
          lh->v.u.ind.idx = extra;  /* previous assignment will use safe copy */
        }
      }
    }
  }
  if (conflict) {
    /* copy upvalue/local value to a temporary (in position 'extra') */
    if (v->k == VLOCAL)
      luaK_codeABC(fs, OP_MOVE, extra, v->u.var.ridx, 0);
    else
      luaK_codeABC(fs, OP_GETUPVAL, extra, v->u.info, 0);
    luaK_reserveregs(fs, 1);
  }
}


/* Create code to store the "top" register in 'var' */
static void storevartop (FuncState *fs, expdesc *var) {
  expdesc e;
  init_exp(&e, VNONRELOC, fs->freereg - 1);
  luaK_storevar(fs, var, &e);  /* will also free the top register */
}


/*
** Parse and compile a multiple assignment. The first "variable"
** (a 'suffixedexp') was already read by the caller.
**
** assignment -> suffixedexp restassign
** restassign -> ',' suffixedexp restassign | '=' explist
*/
static void restassign (LexState *ls, struct LHS_assign *lh, int nvars) {
  expdesc e;
  check_condition(ls, vkisvar(lh->v.k), "syntax error");
  check_readonly(ls, &lh->v);
  if (testnext(ls, ',')) {  /* restassign -> ',' suffixedexp restassign */
    struct LHS_assign nv;
    nv.prev = lh;
    suffixedexp(ls, &nv.v);
    if (!vkisindexed(nv.v.k))
      check_conflict(ls, lh, &nv.v);
    enterlevel(ls);  /* control recursion depth */
    restassign(ls, &nv, nvars+1);
    leavelevel(ls);
  }
  else {  /* restassign -> '=' explist */
    int nexps;
    checknext(ls, '=');
    nexps = explist(ls, &e);
    if (nexps != nvars)
      adjust_assign(ls, nvars, nexps, &e);
    else {
      luaK_setoneret(ls->fs, &e);  /* close last expression */
      luaK_storevar(ls->fs, &lh->v, &e);
      return;  /* avoid default */
    }
  }
  storevartop(ls->fs, &lh->v);  /* default assignment */
}


static int cond (LexState *ls) {
  /* cond -> exp */
  expdesc v;
  expr(ls, &v);  /* read condition */
  if (v.k == VNIL) v.k = VFALSE;  /* 'falses' are all equal here */
  luaK_goiftrue(ls->fs, &v);
  return v.f;
}


static void gotostat (LexState *ls, int line) {
  TString *name = str_checkname(ls);  /* label's name */
  newgotoentry(ls, name, line);
}


/*
** Break statement. Semantically equivalent to "goto break".
*/
static void breakstat (LexState *ls, int line) {
  BlockCnt *bl;  /* to look for an enclosing loop */
  for (bl = ls->fs->bl; bl != NULL; bl = bl->previous) {
    if (bl->isloop)  /* found one? */
      goto ok;
  }
  luaX_syntaxerror(ls, "break outside loop");
 ok:
  bl->isloop = 2;  /* signal that block has pending breaks */
  luaX_next(ls);  /* skip break */
  newgotoentry(ls, ls->brkn, line);
}


/*
** Check whether there is already a label with the given 'name' at
** current function.
*/
static void checkrepeated (LexState *ls, TString *name) {
  Labeldesc *lb = findlabel(ls, name, ls->fs->firstlabel);
  if (l_unlikely(lb != NULL))  /* already defined? */
    luaK_semerror(ls, "label '%s' already defined on line %d",
                      getstr(name), lb->line);  /* error */
}


static void labelstat (LexState *ls, TString *name, int line) {
  /* label -> '::' NAME '::' */
  checknext(ls, TK_DBCOLON);  /* skip double colon */
  while (ls->t.token == ';' || ls->t.token == TK_DBCOLON)
    statement(ls);  /* skip other no-op statements */
  checkrepeated(ls, name);  /* check for repeated labels */
  createlabel(ls, name, line, block_follow(ls, 0));
}


static void whilestat (LexState *ls, int line) {
  /* whilestat -> WHILE cond DO block END */
  FuncState *fs = ls->fs;
  int whileinit;
  int condexit;
  BlockCnt bl;
  luaX_next(ls);  /* skip WHILE */
  whileinit = luaK_getlabel(fs);
  condexit = cond(ls);
  enterblock(fs, &bl, 1);
  checknext(ls, TK_DO);
  block(ls);
  luaK_jumpto(fs, whileinit);
  check_match(ls, TK_END, TK_WHILE, line);
  leaveblock(fs);
  luaK_patchtohere(fs, condexit);  /* false conditions finish the loop */
}


static void repeatstat (LexState *ls, int line) {
  /* repeatstat -> REPEAT block UNTIL cond */
  int condexit;
  FuncState *fs = ls->fs;
  int repeat_init = luaK_getlabel(fs);
  BlockCnt bl1, bl2;
  enterblock(fs, &bl1, 1);  /* loop block */
  enterblock(fs, &bl2, 0);  /* scope block */
  luaX_next(ls);  /* skip REPEAT */
  statlist(ls);
  check_match(ls, TK_UNTIL, TK_REPEAT, line);
  condexit = cond(ls);  /* read condition (inside scope block) */
  leaveblock(fs);  /* finish scope */
  if (bl2.upval) {  /* upvalues? */
    int exit = luaK_jump(fs);  /* normal exit must jump over fix */
    luaK_patchtohere(fs, condexit);  /* repetition must close upvalues */
    luaK_codeABC(fs, OP_CLOSE, reglevel(fs, bl2.nactvar), 0, 0);
    condexit = luaK_jump(fs);  /* repeat after closing upvalues */
    luaK_patchtohere(fs, exit);  /* normal exit comes to here */
  }
  luaK_patchlist(fs, condexit, repeat_init);  /* close the loop */
  leaveblock(fs);  /* finish loop */
}


/*
** Read an expression and generate code to put its results in next
** stack slot.
**
*/
static void exp1 (LexState *ls) {
  expdesc e;
  expr(ls, &e);
  luaK_exp2nextreg(ls->fs, &e);
  lua_assert(e.k == VNONRELOC);
}


/*
** Fix for instruction at position 'pc' to jump to 'dest'.
** (Jump addresses are relative in Lua). 'back' true means
** a back jump.
*/
static void fixforjump (FuncState *fs, int pc, int dest, int back) {
  Instruction *jmp = &fs->f->code[pc];
  int offset = dest - (pc + 1);
  if (back)
    offset = -offset;
  if (l_unlikely(offset > MAXARG_Bx))
    luaX_syntaxerror(fs->ls, "control structure too long");
  SETARG_Bx(*jmp, offset);
}


/*
** Generate code for a 'for' loop.
*/
static void forbody (LexState *ls, int base, int line, int nvars, int isgen) {
  /* forbody -> DO block */
  static const OpCode forprep[2] = {OP_FORPREP, OP_TFORPREP};
  static const OpCode forloop[2] = {OP_FORLOOP, OP_TFORLOOP};
  BlockCnt bl;
  FuncState *fs = ls->fs;
  int prep, endfor;
  checknext(ls, TK_DO);
  prep = luaK_codeABx(fs, forprep[isgen], base, 0);
  fs->freereg--;  /* both 'forprep' remove one register from the stack */
  enterblock(fs, &bl, 0);  /* scope for declared variables */
  adjustlocalvars(ls, nvars);
  luaK_reserveregs(fs, nvars);
  block(ls);
  leaveblock(fs);  /* end of scope for declared variables */
  fixforjump(fs, prep, luaK_getlabel(fs), 0);
  if (isgen) {  /* generic for? */
    luaK_codeABC(fs, OP_TFORCALL, base, 0, nvars);
    luaK_fixline(fs, line);
  }
  endfor = luaK_codeABx(fs, forloop[isgen], base, 0);
  fixforjump(fs, endfor, prep + 1, 1);
  luaK_fixline(fs, line);
}


static void fornum (LexState *ls, TString *varname, int line) {
  /* fornum -> NAME = exp,exp[,exp] forbody */
  FuncState *fs = ls->fs;
  int base = fs->freereg;
  new_localvarliteral(ls, "(for state)");
  new_localvarliteral(ls, "(for state)");
  new_varkind(ls, varname, RDKCONST);  /* control variable */
  checknext(ls, '=');
  exp1(ls);  /* initial value */
  checknext(ls, ',');
  exp1(ls);  /* limit */
  if (testnext(ls, ','))
    exp1(ls);  /* optional step */
  else {  /* default step = 1 */
    luaK_int(fs, fs->freereg, 1);
    luaK_reserveregs(fs, 1);
  }
  adjustlocalvars(ls, 2);  /* start scope for internal variables */
  forbody(ls, base, line, 1, 0);
}


static void forlist (LexState *ls, TString *indexname) {
  /* forlist -> NAME {,NAME} IN explist forbody */
  FuncState *fs = ls->fs;
  expdesc e;
  int nvars = 4;  /* function, state, closing, control */
  int line;
  int base = fs->freereg;
  /* create internal variables */
  new_localvarliteral(ls, "(for state)");  /* iterator function */
  new_localvarliteral(ls, "(for state)");  /* state */
  new_localvarliteral(ls, "(for state)");  /* closing var. (after swap) */
  new_varkind(ls, indexname, RDKCONST);  /* control variable */
  /* other declared variables */
  while (testnext(ls, ',')) {
    new_localvar(ls, str_checkname(ls));
    nvars++;
  }
  checknext(ls, TK_IN);
  line = ls->linenumber;
  adjust_assign(ls, 4, explist(ls, &e), &e);
  adjustlocalvars(ls, 3);  /* start scope for internal variables */
  marktobeclosed(fs);  /* last internal var. must be closed */
  luaK_checkstack(fs, 2);  /* extra space to call iterator */
  forbody(ls, base, line, nvars - 3, 1);
}


static void forstat (LexState *ls, int line) {
  /* forstat -> FOR (fornum | forlist) END */
  FuncState *fs = ls->fs;
  TString *varname;
  BlockCnt bl;
  enterblock(fs, &bl, 1);  /* scope for loop and control variables */
  luaX_next(ls);  /* skip 'for' */
  varname = str_checkname(ls);  /* first variable name */
  switch (ls->t.token) {
    case '=': fornum(ls, varname, line); break;
    case ',': case TK_IN: forlist(ls, varname); break;
    default: luaX_syntaxerror(ls, "'=' or 'in' expected");
  }
  check_match(ls, TK_END, TK_FOR, line);
  leaveblock(fs);  /* loop scope ('break' jumps to this point) */
}


static void test_then_block (LexState *ls, int *escapelist) {
  /* test_then_block -> [IF | ELSEIF] cond THEN block */
  FuncState *fs = ls->fs;
  int condtrue;
  luaX_next(ls);  /* skip IF or ELSEIF */
  condtrue = cond(ls);  /* read condition */
  checknext(ls, TK_THEN);
  block(ls);  /* 'then' part */
  if (ls->t.token == TK_ELSE ||
      ls->t.token == TK_ELSEIF)  /* followed by 'else'/'elseif'? */
    luaK_concat(fs, escapelist, luaK_jump(fs));  /* must jump over it */
  luaK_patchtohere(fs, condtrue);
}


static void ifstat (LexState *ls, int line) {
  /* ifstat -> IF cond THEN block {ELSEIF cond THEN block} [ELSE block] END */
  FuncState *fs = ls->fs;
  int escapelist = NO_JUMP;  /* exit list for finished parts */
  test_then_block(ls, &escapelist);  /* IF cond THEN block */
  while (ls->t.token == TK_ELSEIF)
    test_then_block(ls, &escapelist);  /* ELSEIF cond THEN block */
  if (testnext(ls, TK_ELSE))
    block(ls);  /* 'else' part */
  check_match(ls, TK_END, TK_IF, line);
  luaK_patchtohere(fs, escapelist);  /* patch escape list to 'if' end */
}


static void localfunc (LexState *ls) {
  expdesc b;
  FuncState *fs = ls->fs;
  int fvar = fs->nactvar;  /* function's variable index */
  new_localvar(ls, str_checkname(ls));  /* new local variable */
  adjustlocalvars(ls, 1);  /* enter its scope */
  body(ls, &b, 0, ls->linenumber);  /* function created in next register */
  /* debug information will only see the variable after this point! */
  localdebuginfo(fs, fvar)->startpc = fs->pc;
}


static lu_byte getvarattribute (LexState *ls, lu_byte df) {
  /* attrib -> ['<' NAME '>'] */
  if (testnext(ls, '<')) {
    TString *ts = str_checkname(ls);
    const char *attr = getstr(ts);
    checknext(ls, '>');
    if (strcmp(attr, "const") == 0)
      return RDKCONST;  /* read-only variable */
    else if (strcmp(attr, "close") == 0)
      return RDKTOCLOSE;  /* to-be-closed variable */
    else
      luaK_semerror(ls, "unknown attribute '%s'", attr);
  }
  return df;  /* return default value */
}


static void checktoclose (FuncState *fs, int level) {
  if (level != -1) {  /* is there a to-be-closed variable? */
    marktobeclosed(fs);
    luaK_codeABC(fs, OP_TBC, reglevel(fs, level), 0, 0);
  }
}


static void localstat (LexState *ls) {
  /* stat -> LOCAL NAME [':' type] attrib { ',' NAME [':' type] attrib } ['=' explist] */
  FuncState *fs = ls->fs;
  int toclose = -1;  /* index of to-be-closed variable (if any) */
  Vardesc *var;  /* last variable */
  int vidx;  /* index of last variable */
  int nvars = 0;
  int nexps;
  expdesc e;
  int firstvar;  /* index of first variable in this declaration */
  /* get prefixed attribute (if any); default is regular local variable */
  lu_byte defkind = getvarattribute(ls, VDKREG);
  firstvar = fs->nactvar;
  do {  /* for each variable */
    TString *vname = str_checkname(ls);  /* get its name */
    TString *typanno = optional_type_annotation(ls);  /* optional ': type' */
    lu_byte kind = getvarattribute(ls, defkind);  /* postfixed attribute */
    vidx = new_varkind(ls, vname, kind);  /* predeclare it */
    /* store type annotation in Vardesc */
    if (typanno != NULL)
      getlocalvardesc(fs, vidx)->vd.type_annotation = typanno;
    if (kind == RDKTOCLOSE) {  /* to-be-closed? */
      if (toclose != -1)  /* one already present? */
        luaK_semerror(ls, "multiple to-be-closed variables in local list");
      toclose = fs->nactvar + nvars;
    }
    nvars++;
  } while (testnext(ls, ','));
  if (testnext(ls, '='))  /* initialization? */
    nexps = explist(ls, &e);
  else {
    e.k = VVOID;
    nexps = 0;
  }
  var = getlocalvardesc(fs, vidx);  /* retrieve last variable */
  if (nvars == nexps &&  /* no adjustments? */
      var->vd.kind == RDKCONST &&  /* last variable is const? */
      luaK_exp2const(fs, &e, &var->k)) {  /* compile-time constant? */
    var->vd.kind = RDKCTC;  /* variable is a compile-time constant */
    adjustlocalvars(ls, nvars - 1);  /* exclude last variable */
    fs->nactvar++;  /* but count it */
  }
  else {
    adjust_assign(ls, nvars, nexps, &e);
    adjustlocalvars(ls, nvars);
  }
  checktoclose(fs, toclose);
  /* emit OP_TYPECHECK for typed variables (after assignment) */
  if (nexps > 0) {  /* only if there are initializers */
    int i;
    for (i = 0; i < nvars; i++) {
      Vardesc *v = getlocalvardesc(fs, firstvar + i);
      if (v->vd.type_annotation != NULL) {
        const char *expected = getstr(v->vd.type_annotation);
        /* for single-var assignments, check literal type at compile time */
        if (nvars == 1 && nexps == 1) {
          const char *actual = expr_compiletime_type(e.k);
          if (actual != NULL && strcmp(expected, "unknown") != 0) {
            if (strcmp(expected, actual) != 0)
              luaK_semerror(ls,
                "type error: '%s' expected for variable '%s', got '%s'",
                expected, getstr(v->vd.name), actual);
          }
        }
        /* emit OP_TYPECHECK for dynamic values (function calls etc) */
        int reg = v->vd.ridx;
        int tid = get_typeid(expected);
        if (tid >= 0) {
          /* built-in type: use type ID for fast check (no strcmp) */
          luaK_codeABC(fs, OP_TYPECHECK, reg, tid, 0);
        }
        else {
          /* class type: use TYPEID_CLASS + K index for class name */
          int kk = luaK_stringK(fs, v->vd.type_annotation);
          luaK_codeABC(fs, OP_TYPECHECK, reg, TYPEID_CLASS, kk);
        }
      }
    }
  }
}


static lu_byte getglobalattribute (LexState *ls, lu_byte df) {
  lu_byte kind = getvarattribute(ls, df);
  switch (kind) {
    case RDKTOCLOSE:
      luaK_semerror(ls, "global variables cannot be to-be-closed");
      return kind;  /* to avoid warnings */
    case RDKCONST:
      return GDKCONST;  /* adjust kind for global variable */
    default:
      return kind;
  }
}


static void checkglobal (LexState *ls, TString *varname, int line) {
  FuncState *fs = ls->fs;
  expdesc var;
  int k;
  buildglobal(ls, varname, &var);  /* create global variable in 'var' */
  k = var.u.ind.keystr;  /* index of global name in 'k' */
  luaK_codecheckglobal(fs, &var, k, line);
}


/*
** Recursively traverse list of globals to be initalized. When
** going, generate table description for the global. In the end,
** after all indices have been generated, read list of initializing
** expressions. When returning, generate the assignment of the value on
** the stack to the corresponding table description. 'n' is the variable
** being handled, range [0, nvars - 1].
*/
static void initglobal (LexState *ls, int nvars, int firstidx, int n,
                        int line) {
  if (n == nvars) {  /* traversed all variables? */
    expdesc e;
    int nexps = explist(ls, &e);  /* read list of expressions */
    adjust_assign(ls, nvars, nexps, &e);
  }
  else {  /* handle variable 'n' */
    FuncState *fs = ls->fs;
    expdesc var;
    TString *varname = getlocalvardesc(fs, firstidx + n)->vd.name;
    buildglobal(ls, varname, &var);  /* create global variable in 'var' */
    enterlevel(ls);  /* control recursion depth */
    initglobal(ls, nvars, firstidx, n + 1, line);
    leavelevel(ls);
    checkglobal(ls, varname, line);
    storevartop(fs, &var);
  }
}


static void globalnames (LexState *ls, lu_byte defkind) {
  FuncState *fs = ls->fs;
  int nvars = 0;
  int lastidx;  /* index of last registered variable */
  do {  /* for each name */
    TString *vname = str_checkname(ls);
    optional_type_annotation(ls);  /* skip optional ': type' */
    lu_byte kind = getglobalattribute(ls, defkind);
    lastidx = new_varkind(ls, vname, kind);
    nvars++;
  } while (testnext(ls, ','));
  if (testnext(ls, '='))  /* initialization? */
    initglobal(ls, nvars, lastidx - nvars + 1, 0, ls->linenumber);
  fs->nactvar = cast_short(fs->nactvar + nvars);  /* activate declaration */
}


static void globalstat (LexState *ls) {
  /* globalstat -> (GLOBAL) attrib '*'
     globalstat -> (GLOBAL) attrib NAME attrib {',' NAME attrib} */
  FuncState *fs = ls->fs;
  /* get prefixed attribute (if any); default is regular global variable */
  lu_byte defkind = getglobalattribute(ls, GDKREG);
  if (!testnext(ls, '*'))
    globalnames(ls, defkind);
  else {
    /* use NULL as name to represent '*' entries */
    new_varkind(ls, NULL, defkind);
    fs->nactvar++;  /* activate declaration */
  }
}


static void globalfunc (LexState *ls, int line) {
  /* globalfunc -> (GLOBAL FUNCTION) NAME body */
  expdesc var, b;
  FuncState *fs = ls->fs;
  TString *fname = str_checkname(ls);
  new_varkind(ls, fname, GDKREG);  /* declare global variable */
  fs->nactvar++;  /* enter its scope */
  buildglobal(ls, fname, &var);
  body(ls, &b, 0, ls->linenumber);  /* compile and return closure in 'b' */
  checkglobal(ls, fname, line);
  luaK_storevar(fs, &var, &b);
  luaK_fixline(fs, line);  /* definition "happens" in the first line */
}


static void globalstatfunc (LexState *ls, int line) {
  /* stat -> GLOBAL globalfunc | GLOBAL globalstat */
  luaX_next(ls);  /* skip 'global' */
  if (testnext(ls, TK_FUNCTION))
    globalfunc(ls, line);
  else
    globalstat(ls);
}


static int funcname (LexState *ls, expdesc *v) {
  /* funcname -> NAME {fieldsel} [':' NAME] */
  int ismethod = 0;
  singlevar(ls, v);
  while (ls->t.token == '.')
    fieldsel(ls, v);
  if (ls->t.token == ':') {
    ismethod = 1;
    fieldsel(ls, v);
  }
  return ismethod;
}


static void funcstat (LexState *ls, int line) {
  /* funcstat -> FUNCTION funcname body */
  int ismethod;
  expdesc v, b;
  luaX_next(ls);  /* skip FUNCTION */
  ismethod = funcname(ls, &v);
  check_readonly(ls, &v);
  body(ls, &b, ismethod, line);
  luaK_storevar(ls->fs, &v, &b);
  luaK_fixline(ls->fs, line);  /* definition "happens" in the first line */
}


static void exprstat (LexState *ls) {
  /* stat -> func | assignment */
  FuncState *fs = ls->fs;
  struct LHS_assign v;
  suffixedexp(ls, &v.v);
  if (ls->t.token == '=' || ls->t.token == ',') { /* stat -> assignment ? */
    v.prev = NULL;
    restassign(ls, &v, 1);
  }
  else {  /* stat -> func */
    Instruction *inst;
    check_condition(ls, v.v.k == VCALL, "syntax error");
    inst = &getinstruction(fs, &v.v);
    SETARG_C(*inst, 1);  /* call statement uses no results */
  }
}


static void retstat (LexState *ls) {
  /* stat -> RETURN [explist] [';'] */
  FuncState *fs = ls->fs;
  expdesc e;
  int nret;  /* number of values being returned */
  int first = luaY_nvarstack(fs);  /* first slot to be returned */
  if (block_follow(ls, 1) || ls->t.token == ';')
    nret = 0;  /* return no values */
  else {
    nret = explist(ls, &e);  /* optional return values */
    if (hasmultret(e.k)) {
      luaK_setmultret(fs, &e);
      if (e.k == VCALL && nret == 1 && !fs->bl->insidetbc) {  /* tail call? */
        SET_OPCODE(getinstruction(fs,&e), OP_TAILCALL);
        lua_assert(GETARG_A(getinstruction(fs,&e)) == luaY_nvarstack(fs));
      }
      nret = LUA_MULTRET;  /* return all values */
    }
    else {
      if (nret == 1)  /* only one single value? */
        first = luaK_exp2anyreg(fs, &e);  /* can use original slot */
      else {  /* values must go to the top of the stack */
        luaK_exp2nextreg(fs, &e);
        lua_assert(nret == fs->freereg - first);
      }
    }
  }
  luaK_ret(fs, first, nret);
  testnext(ls, ';');  /* skip optional semicolon */
}


/*
** =======================================================
** Type alias statement: type Name = ...
** =======================================================
*/
static void typestat (LexState *ls) {
  /* typestat -> 'type' NAME '=' type */
  /* Note: 'type' is already consumed as a TK_NAME, and lookahead confirmed
     the next token is TK_NAME. The lookahead has been consumed, so
     current token is now the type alias name. */
  luaX_next(ls);  /* skip 'type' (which is a TK_NAME) */
  str_checkname(ls);  /* skip type alias name */
  checknext(ls, '=');
  parse_type(ls);  /* skip type definition (parsed and discarded) */
}


/*
** =======================================================
** Interface declaration
** =======================================================
*/

/*
** Register an interface in the parser's registry.
*/
static void register_interface (LexState *ls, TString *name) {
  lua_State *L = ls->L;
  if (ls->ninterfaces >= ls->interfaces_size) {
    int newsize = (ls->interfaces_size == 0) ? 4 : ls->interfaces_size * 2;
    ls->interfaces = luaM_reallocvector(L, ls->interfaces,
                       ls->interfaces_size, newsize, struct InterfaceInfo);
    ls->interfaces_size = newsize;
  }
  struct InterfaceInfo *iface = &ls->interfaces[ls->ninterfaces++];
  iface->name = name;
  iface->methods = NULL;
  iface->nmethods = 0;
  iface->methods_size = 0;
}

static struct InterfaceInfo *find_interface (LexState *ls, TString *name) {
  for (int i = 0; i < ls->ninterfaces; i++) {
    if (ls->interfaces[i].name == name)
      return &ls->interfaces[i];
  }
  return NULL;
}

static void interface_add_method (LexState *ls, struct InterfaceInfo *iface,
                                   TString *method) {
  lua_State *L = ls->L;
  if (iface->nmethods >= iface->methods_size) {
    int newsize = (iface->methods_size == 0) ? 8 : iface->methods_size * 2;
    iface->methods = luaM_reallocvector(L, iface->methods,
                       iface->methods_size, newsize, TString *);
    iface->methods_size = newsize;
  }
  iface->methods[iface->nmethods++] = method;
}


/*
** interfacestat -> INTERFACE NAME { FUNCTION NAME '(' parlist ')' [':' type] }
**                  END
** Parsed at compile time only. Stores method signatures for validation
** when a class uses 'implements'.
*/
static void interfacestat (LexState *ls, int line) {
  TString *ifname;
  struct InterfaceInfo *iface;
  luaX_next(ls);  /* skip 'interface' */
  ifname = str_checkname(ls);
  register_interface(ls, ifname);
  /* also register as a valid type name */
  register_classname(ls, ifname);
  iface = find_interface(ls, ifname);

  /* parse interface body: method declarations (no bodies) */
  while (ls->t.token != TK_END && ls->t.token != TK_EOS) {
    if (ls->t.token == TK_FUNCTION) {
      luaX_next(ls);  /* skip 'function' */
      TString *mname = str_checkname(ls);
      interface_add_method(ls, iface, mname);
      /* parse parameter list (just skip it) */
      checknext(ls, '(');
      while (ls->t.token != ')' && ls->t.token != TK_EOS) {
        if (ls->t.token == TK_NAME) {
          luaX_next(ls);  /* skip param name */
          optional_type_annotation(ls);
        }
        else if (ls->t.token == TK_DOTS)
          luaX_next(ls);
        if (ls->t.token == ',') luaX_next(ls);
      }
      checknext(ls, ')');
      optional_type_annotation(ls);  /* return type */
      /* no body - just declaration */
    }
    else if (ls->t.token == TK_NAME) {
      /* field declaration: NAME ':' type */
      luaX_next(ls);
      if (ls->t.token == ':') {
        luaX_next(ls);
        parse_type(ls);
      }
    }
    else if (ls->t.token == ';') {
      luaX_next(ls);
    }
    else {
      luaX_syntaxerror(ls, "'function' or 'end' expected in interface body");
    }
  }
  check_match(ls, TK_END, TK_INTERFACE, line);
}


/*
** Check that a class implements all methods required by an interface.
** Called at compile time after the class body is parsed.
*/
static void check_implements (LexState *ls, TString *classname,
                               TString *ifacename,
                               TString **class_methods, int nclass_methods) {
  struct InterfaceInfo *iface = find_interface(ls, ifacename);
  if (iface == NULL)
    luaK_semerror(ls, "unknown interface '%s'", getstr(ifacename));
  for (int i = 0; i < iface->nmethods; i++) {
    TString *required = iface->methods[i];
    int found = 0;
    for (int j = 0; j < nclass_methods; j++) {
      if (class_methods[j] == required) { found = 1; break; }
    }
    if (!found)
      luaK_semerror(ls, "class '%s' missing method '%s' required by interface '%s'",
                    getstr(classname), getstr(required), getstr(ifacename));
  }
}


/*
** =======================================================
** Enum declaration
** =======================================================
*/

/*
** enumstat -> ENUM NAME { NAME ['=' expr] {',' NAME ['=' expr]} } END
** Generates: EnumName = {VALUE1 = 1, VALUE2 = 2, ...}
*/
static void enumstat (LexState *ls, int line) {
  FuncState *fs = ls->fs;
  TString *enumname;
  expdesc var, val;
  int pc;
  int counter = 1;

  luaX_next(ls);  /* skip 'enum' */
  enumname = str_checkname(ls);

  /* register as valid type name */
  register_classname(ls, enumname);

  /* Generate: EnumName = {} */
  buildglobal(ls, enumname, &var);
  pc = luaK_codevABCk(fs, OP_NEWTABLE, 0, 0, 0, 0);
  luaK_code(fs, 0);
  init_exp(&val, VNONRELOC, fs->freereg);
  luaK_reserveregs(fs, 1);
  luaK_settablesize(fs, pc, val.u.info, 0, 0);
  luaK_storevar(fs, &var, &val);
  luaK_fixline(fs, line);

  /* parse enum values */
  while (ls->t.token != TK_END && ls->t.token != TK_EOS) {
    if (ls->t.token == TK_NAME) {
      TString *vname = str_checkname(ls);
      expdesc tab, key, vv;

      if (testnext(ls, '=')) {
        /* explicit value */
        expr(ls, &vv);
      }
      else {
        /* auto-increment */
        init_exp(&vv, VKINT, 0);
        vv.u.ival = counter;
      }
      counter++;

      /* EnumName.VALUE = val */
      buildglobal(ls, enumname, &tab);
      luaK_exp2anyregup(fs, &tab);
      codestring(&key, vname);
      luaK_indexed(fs, &tab, &key);
      luaK_storevar(fs, &tab, &vv);
      luaK_fixline(fs, ls->linenumber);

      testnext(ls, ',');  /* optional comma */
    }
    else if (ls->t.token == ';') {
      luaX_next(ls);
    }
    else {
      luaX_syntaxerror(ls, "name or 'end' expected in enum body");
    }
  }
  check_match(ls, TK_END, TK_ENUM, line);
}


/*
** =======================================================
** Class declaration statement
** =======================================================
*/


/*
** Parse a class body method: function NAME '(' parlist ')' block end
** Generates: ClassName.methodName = function(self, ...) ... end
*/
static TString *classmethod (LexState *ls, expdesc *classvar, TString *cname) {
  /* classmethod -> FUNCTION NAME body */
  FuncState *fs = ls->fs;
  int line = ls->linenumber;
  expdesc key, b;
  TString *methodname;
  luaX_next(ls);  /* skip 'function' */
  methodname = str_checkname(ls);
  codestring(&key, methodname);
  /* make a copy of classvar for indexing */
  expdesc tab = *classvar;
  luaK_exp2anyregup(fs, &tab);
  luaK_indexed(fs, &tab, &key);  /* tab = ClassName[methodName] */
  /* parse function body with class context set */
  /* body() will call open_func which creates a new FuncState;
     we set classctx on that new FuncState after open_func via
     a temporary stored in ls */
  TString *saved_classctx = ls->fs->classctx;
  /* The body() function will create a new FuncState. We need to set
     classctx on the NEW FuncState. Use a trick: set it on the current
     fs, and body's open_func will inherit it from prev. */
  body_classctx(ls, &b, 0, line, cname);
  ls->fs->classctx = saved_classctx;  /* restore */
  luaK_storevar(fs, &tab, &b);
  luaK_fixline(fs, line);
  return methodname;
}


/*
** classstat -> CLASS NAME [EXTENDS NAME] classblock END
** classblock -> { fielddecl | classmethod }
** fielddecl -> NAME ':' type
**
** Generates code equivalent to:
**   ClassName = {}
**   ClassName.__index = ClassName
**   [if extends: setmetatable(ClassName, {__index = ParentName})]
**   ClassName.method1 = function(...) ... end
**   ...
*/
static void classstat (LexState *ls, int line) {
  FuncState *fs = ls->fs;
  TString *classname;
  expdesc classvar, val;
  int hasparent = 0;

  luaX_next(ls);  /* skip 'class' */
  classname = str_checkname(ls);  /* get class name */

  /* register class name for compile-time type validation */
  register_classname(ls, classname);

  /* Check for 'extends' */
  TString *parentname = NULL;
  if (ls->t.token == TK_EXTENDS) {
    luaX_next(ls);  /* skip 'extends' */
    parentname = str_checkname(ls);  /* get parent class name */
    hasparent = 1;
  }
  /* register parent relationship for override checking */
  register_classparent(ls, classname, parentname);

  /* Check for 'implements' (one or more interfaces) */
  #define MAX_IMPLEMENTS 8
  TString *impl_ifaces[MAX_IMPLEMENTS];
  int nimpl = 0;
  if (ls->t.token == TK_IMPLEMENTS) {
    luaX_next(ls);  /* skip 'implements' */
    do {
      if (nimpl >= MAX_IMPLEMENTS)
        luaK_semerror(ls, "too many interfaces");
      impl_ifaces[nimpl++] = str_checkname(ls);
    } while (testnext(ls, ','));
  }

  /*
  ** Generate: ClassName = {}
  ** Use buildglobal to get the _ENV[ClassName] target
  */
  {
    int pc;
    buildglobal(ls, classname, &classvar);

    /* Create empty table: {} */
    pc = luaK_codevABCk(fs, OP_NEWTABLE, 0, 0, 0, 0);
    luaK_code(fs, 0);  /* space for extra arg */
    init_exp(&val, VNONRELOC, fs->freereg);
    luaK_reserveregs(fs, 1);
    luaK_settablesize(fs, pc, val.u.info, 0, 0);

    /* ClassName = {} */
    luaK_storevar(fs, &classvar, &val);
    luaK_fixline(fs, line);
  }

  /*
  ** Generate: ClassName.__index = ClassName
  */
  {
    expdesc tab, key, self;
    buildglobal(ls, classname, &tab);
    luaK_exp2anyregup(fs, &tab);
    codestring(&key, luaX_newstring(ls, "__index", 7));
    luaK_indexed(fs, &tab, &key);

    buildglobal(ls, classname, &self);
    luaK_storevar(fs, &tab, &self);
    luaK_fixline(fs, line);
  }

  /*
  ** If extends: setmetatable(ClassName, {__index = ParentName})
  ** Generate the equivalent bytecode:
  **   local _tmp = {}
  **   _tmp.__index = ParentName
  **   setmetatable(ClassName, _tmp)
  */
  if (hasparent) {
    expdesc setmt, arg1, tmptab;
    int pc;
    int base;

    /* Get setmetatable from _ENV */
    buildglobal(ls, luaX_newstring(ls, "setmetatable", 12), &setmt);
    luaK_exp2nextreg(fs, &setmt);
    base = fs->freereg - 1;

    /* First arg: ClassName */
    buildglobal(ls, classname, &arg1);
    luaK_exp2nextreg(fs, &arg1);

    /* Second arg: {__index = ParentName} */
    pc = luaK_codevABCk(fs, OP_NEWTABLE, 0, 0, 0, 0);
    luaK_code(fs, 0);  /* space for extra arg */
    init_exp(&tmptab, VNONRELOC, fs->freereg);
    luaK_reserveregs(fs, 1);
    luaK_settablesize(fs, pc, tmptab.u.info, 0, 1);

    /* Set __index field in the temp table */
    {
      expdesc idx, pval;
      expdesc t2 = tmptab;
      codestring(&idx, luaX_newstring(ls, "__index", 7));
      luaK_indexed(fs, &t2, &idx);
      buildglobal(ls, parentname, &pval);
      luaK_storevar(fs, &t2, &pval);
    }

    /* Call setmetatable(ClassName, {__index=Parent}) */
    init_exp(&setmt, VCALL, luaK_codeABC(fs, OP_CALL, base, 3, 2));
    luaK_fixline(fs, line);
    fs->freereg = cast_byte(base + 1);

    /* Store result back: ClassName = setmetatable result */
    {
      expdesc dest;
      buildglobal(ls, classname, &dest);
      luaK_storevar(fs, &dest, &setmt);
    }
    luaK_fixline(fs, line);
  }

  /*
  ** Parse class body: field declarations, methods, and properties
  ** with access modifiers (public/private/protected/readonly)
  */
  /* Track if we need access control setup */
  int has_access_control = 0;

  /* Pre-create __getters and __setters tables on the class */
  {
    expdesc cv, key, tab;
    int pc;
    /* ClassName.__getters = {} */
    buildglobal(ls, classname, &cv);
    luaK_exp2anyregup(fs, &cv);
    codestring(&key, luaX_newstring(ls, "__getters", 9));
    luaK_indexed(fs, &cv, &key);
    pc = luaK_codevABCk(fs, OP_NEWTABLE, 0, 0, 0, 0);
    luaK_code(fs, 0);
    init_exp(&tab, VNONRELOC, fs->freereg);
    luaK_reserveregs(fs, 1);
    luaK_settablesize(fs, pc, tab.u.info, 0, 0);
    luaK_storevar(fs, &cv, &tab);

    /* ClassName.__setters = {} */
    buildglobal(ls, classname, &cv);
    luaK_exp2anyregup(fs, &cv);
    codestring(&key, luaX_newstring(ls, "__setters", 9));
    luaK_indexed(fs, &cv, &key);
    pc = luaK_codevABCk(fs, OP_NEWTABLE, 0, 0, 0, 0);
    luaK_code(fs, 0);
    init_exp(&tab, VNONRELOC, fs->freereg);
    luaK_reserveregs(fs, 1);
    luaK_settablesize(fs, pc, tab.u.info, 0, 0);
    luaK_storevar(fs, &cv, &tab);
  }

  /* field access info: stored as parallel arrays */
  #define MAX_CLASS_FIELDS 64
  TString *field_names[MAX_CLASS_FIELDS];
  const char *field_access[MAX_CLASS_FIELDS];
  int nfields = 0;

  /* getter/setter names for properties */
  TString *getter_names[MAX_CLASS_FIELDS];
  TString *setter_names[MAX_CLASS_FIELDS];
  int ngetters = 0;
  int nsetters = 0;

  /* track method names for implements checking */
  #define MAX_CLASS_METHODS 64
  TString *class_method_names[MAX_CLASS_METHODS];
  int nclass_methods = 0;

  int is_abstract_class = 0;  /* set if any abstract method found */

  while (ls->t.token != TK_END && ls->t.token != TK_EOS) {
    if (ls->t.token == TK_FUNCTION) {
      /* Method declaration - check override requirement */
      expdesc cv;
      buildglobal(ls, classname, &cv);
      /* peek at method name to check override */
      int saved_tok = ls->t.token;
      luaX_next(ls);  /* skip 'function' */
      TString *peekname = ls->t.seminfo.ts;
      /* check: does this method exist in parent? */
      if (hasparent && method_exists_in_parent(ls, classname, peekname))
        luaK_semerror(ls,
          "method '%s' exists in parent class; use 'override function %s' to override",
          getstr(peekname), getstr(peekname));
      /* put tokens back and let classmethod handle normally */
      /* Actually classmethod expects to skip 'function' itself,
         but we already skipped it. Push name back via unread... */
      /* Simpler: just call the rest of classmethod inline */
      {
        int line = ls->linenumber;
        expdesc key, b;
        TString *methodname = str_checkname(ls);
        codestring(&key, methodname);
        expdesc tab = cv;
        luaK_exp2anyregup(fs, &tab);
        luaK_indexed(fs, &tab, &key);
        TString *saved = ls->fs->classctx;
        body_classctx(ls, &b, 0, line, classname);
        ls->fs->classctx = saved;
        luaK_storevar(fs, &tab, &b);
        luaK_fixline(fs, line);
        /* register method */
        register_classmethod_info(ls, classname, methodname);
        if (nclass_methods < MAX_CLASS_METHODS)
          class_method_names[nclass_methods++] = methodname;
      }
    }
    else if (ls->t.token == TK_NAME) {
      /* Check for 'static', 'abstract', 'operator', 'override' keywords */
      const char *kw = getstr(ls->t.seminfo.ts);

      /* override function NAME(...) - explicit parent method override */
      if (strcmp(kw, "override") == 0) {
        luaX_next(ls);  /* skip 'override' */
        if (ls->t.token != TK_FUNCTION)
          luaX_syntaxerror(ls, "'function' expected after 'override'");
        luaX_next(ls);  /* skip 'function' */
        TString *mname = ls->t.seminfo.ts;
        /* verify method DOES exist in parent */
        if (!method_exists_in_parent(ls, classname, mname))
          luaK_semerror(ls,
            "override '%s': method not found in parent class",
            getstr(mname));
        /* compile as normal method */
        expdesc cv;
        buildglobal(ls, classname, &cv);
        {
          int line = ls->linenumber;
          expdesc key, b;
          TString *methodname = str_checkname(ls);
          codestring(&key, methodname);
          expdesc tab = cv;
          luaK_exp2anyregup(fs, &tab);
          luaK_indexed(fs, &tab, &key);
          TString *saved = ls->fs->classctx;
          body_classctx(ls, &b, 0, line, classname);
          ls->fs->classctx = saved;
          luaK_storevar(fs, &tab, &b);
          luaK_fixline(fs, line);
          register_classmethod_info(ls, classname, methodname);
          if (nclass_methods < MAX_CLASS_METHODS)
            class_method_names[nclass_methods++] = methodname;
        }
        continue;
      }

      /* static function NAME(...) - stored on class table directly */
      if (strcmp(kw, "static") == 0) {
        luaX_next(ls);  /* skip 'static' */
        if (ls->t.token == TK_FUNCTION) {
          expdesc cv;
          buildglobal(ls, classname, &cv);
          /* classmethod without class context (no self access to private) */
          TString *mname = classmethod(ls, &cv, NULL);
          if (nclass_methods < MAX_CLASS_METHODS)
            class_method_names[nclass_methods++] = mname;
        }
        else {
          luaX_syntaxerror(ls, "'function' expected after 'static'");
        }
        continue;
      }

      /* abstract function NAME(...) - no body, just declaration */
      if (strcmp(kw, "abstract") == 0) {
        luaX_next(ls);  /* skip 'abstract' */
        if (ls->t.token == TK_FUNCTION) {
          luaX_next(ls);  /* skip 'function' */
          TString *mname = str_checkname(ls);
          /* skip param list */
          checknext(ls, '(');
          while (ls->t.token != ')' && ls->t.token != TK_EOS) {
            if (ls->t.token == TK_NAME) {
              luaX_next(ls);
              optional_type_annotation(ls);
            }
            else if (ls->t.token == TK_DOTS)
              luaX_next(ls);
            if (ls->t.token == ',') luaX_next(ls);
          }
          checknext(ls, ')');
          optional_type_annotation(ls);
          /* no body - abstract */
          is_abstract_class = 1;
          if (nclass_methods < MAX_CLASS_METHODS)
            class_method_names[nclass_methods++] = mname;
        }
        else {
          luaX_syntaxerror(ls, "'function' expected after 'abstract'");
        }
        continue;
      }

      /* operator + - * / == < (maps to __add etc) */
      if (strcmp(kw, "operator") == 0) {
        luaX_next(ls);  /* skip 'operator' */
        /* get the operator symbol */
        TString *metamethod = NULL;
        switch (ls->t.token) {
          case '+': metamethod = luaX_newstring(ls, "__add", 5); break;
          case '-': metamethod = luaX_newstring(ls, "__sub", 5); break;
          case '*': metamethod = luaX_newstring(ls, "__mul", 5); break;
          case '/': metamethod = luaX_newstring(ls, "__div", 5); break;
          case '%': metamethod = luaX_newstring(ls, "__mod", 5); break;
          case TK_EQ: metamethod = luaX_newstring(ls, "__eq", 4); break;
          case '<': metamethod = luaX_newstring(ls, "__lt", 4); break;
          case TK_LE: metamethod = luaX_newstring(ls, "__le", 4); break;
          case TK_CONCAT: metamethod = luaX_newstring(ls, "__concat", 8); break;
          default:
            if (ls->t.token == TK_NAME) {
              const char *opn = getstr(ls->t.seminfo.ts);
              if (strcmp(opn, "len") == 0)
                metamethod = luaX_newstring(ls, "__len", 5);
              else if (strcmp(opn, "tostring") == 0)
                metamethod = luaX_newstring(ls, "__tostring", 10);
              else if (strcmp(opn, "call") == 0)
                metamethod = luaX_newstring(ls, "__call", 6);
            }
            if (metamethod == NULL)
              luaX_syntaxerror(ls, "unknown operator for overloading");
        }
        luaX_next(ls);  /* skip operator token */
        /* parse function body: ClassName.__metamethod = function(...) ... end */
        {
          expdesc cv, key, b;
          buildglobal(ls, classname, &cv);
          luaK_exp2anyregup(fs, &cv);
          codestring(&key, metamethod);
          luaK_indexed(fs, &cv, &key);
          body_classctx(ls, &b, 0, ls->linenumber, classname);
          luaK_storevar(fs, &cv, &b);
          luaK_fixline(fs, ls->linenumber);
        }
        continue;
      }
      TString *word = ls->t.seminfo.ts;
      const char *ws = getstr(word);
      /* check for access modifiers */
      const char *access = NULL;
      int is_readonly = 0;
      if (strcmp(ws, "public") == 0 || strcmp(ws, "private") == 0 ||
          strcmp(ws, "protected") == 0 || strcmp(ws, "readonly") == 0) {
        if (strcmp(ws, "readonly") == 0) {
          access = "readonly";
          is_readonly = 1;
        }
        else
          access = ws;
        luaX_next(ls);  /* skip modifier */
        /* check for 'readonly' after public/private/protected */
        if (!is_readonly && ls->t.token == TK_NAME) {
          const char *nxt = getstr(ls->t.seminfo.ts);
          if (strcmp(nxt, "readonly") == 0) {
            /* e.g. "private readonly x: number" - use private */
            luaX_next(ls);  /* skip 'readonly' */
          }
        }
        has_access_control = 1;
      }

      /* check for 'property' keyword */
      if (ls->t.token == TK_NAME &&
          strcmp(getstr(ls->t.seminfo.ts), "property") == 0 &&
          access != NULL) {
        /* access modifier before property - skip to property handling */
        /* fall through to property check below */
      }

      if (ls->t.token == TK_NAME &&
          strcmp(getstr(ls->t.seminfo.ts), "property") == 0) {
        /* property NAME
             get(self) ... end
             set(self, v) ... end
           end */
        luaX_next(ls);  /* skip 'property' */
        TString *propname = str_checkname(ls);
        optional_type_annotation(ls);  /* optional ': type' */
        has_access_control = 1;

        /* parse get/set blocks until 'end' */
        while (ls->t.token != TK_END && ls->t.token != TK_EOS) {
          if (ls->t.token == TK_NAME) {
            const char *gs = getstr(ls->t.seminfo.ts);
            if (strcmp(gs, "get") == 0) {
              /* get(self) block end */
              expdesc cv, key, b;
              luaX_next(ls);  /* skip 'get' */
              /* Store as ClassName.__getters.propname = function ... */
              buildglobal(ls, classname, &cv);
              luaK_exp2anyregup(fs, &cv);
              codestring(&key, luaX_newstring(ls, "__getters", 9));
              luaK_indexed(fs, &cv, &key);
              /* Now cv = ClassName.__getters */
              luaK_exp2anyregup(fs, &cv);
              codestring(&key, propname);
              luaK_indexed(fs, &cv, &key);
              /* cv = ClassName.__getters[propname] */
              body_classctx(ls, &b, 0, ls->linenumber, classname);
              luaK_storevar(fs, &cv, &b);
              luaK_fixline(fs, ls->linenumber);
              if (ngetters < MAX_CLASS_FIELDS)
                getter_names[ngetters++] = propname;
            }
            else if (strcmp(gs, "set") == 0) {
              expdesc cv, key, b;
              luaX_next(ls);  /* skip 'set' */
              buildglobal(ls, classname, &cv);
              luaK_exp2anyregup(fs, &cv);
              codestring(&key, luaX_newstring(ls, "__setters", 9));
              luaK_indexed(fs, &cv, &key);
              luaK_exp2anyregup(fs, &cv);
              codestring(&key, propname);
              luaK_indexed(fs, &cv, &key);
              body_classctx(ls, &b, 0, ls->linenumber, classname);
              luaK_storevar(fs, &cv, &b);
              luaK_fixline(fs, ls->linenumber);
              if (nsetters < MAX_CLASS_FIELDS)
                setter_names[nsetters++] = propname;
            }
            else break;
          }
          else break;
        }
        checknext(ls, TK_END);  /* property ... end */
      }
      else if (ls->t.token == TK_NAME) {
        /* Field declaration: [modifier] NAME ':' type */
        TString *fname = ls->t.seminfo.ts;
        luaX_next(ls);  /* skip field name */
        if (ls->t.token == ':') {
          luaX_next(ls);  /* skip ':' */
          parse_type(ls);  /* skip type */
        }
        /* store access info for runtime and compile-time checking */
        if (access != NULL && nfields < MAX_CLASS_FIELDS) {
          field_names[nfields] = fname;
          field_access[nfields] = access;
          nfields++;
          /* register for compile-time access validation */
          lu_byte acc_code = 0;
          if (strcmp(access, "private") == 0) acc_code = ACCESS_PRIVATE;
          else if (strcmp(access, "protected") == 0) acc_code = ACCESS_PROTECTED;
          else if (strcmp(access, "readonly") == 0) acc_code = ACCESS_READONLY;
          if (acc_code > 0)
            register_classfield(ls, classname, fname, acc_code);
        }
      }
      else if (access != NULL) {
        /* modifier without field name - might be followed by function */
        if (ls->t.token == TK_FUNCTION) {
          expdesc cv;
          buildglobal(ls, classname, &cv);
          classmethod(ls, &cv, classname);
        }
        else {
          luaX_syntaxerror(ls, "field name or 'function' expected after modifier");
        }
      }
      else {
        luaX_syntaxerror(ls, "':' expected after field name in class body");
      }
    }
    else if (ls->t.token == ';') {
      luaX_next(ls);  /* skip optional semicolons */
    }
    else {
      luaX_syntaxerror(ls,
        "'function', field declaration, modifier, or 'end' expected");
    }
  }

  /*
  ** Generate access control metadata and call __setup_class
  */
  if (has_access_control) {
    int i;

    /* Create ClassName.__access = {field1 = "access", ...} */
    if (nfields > 0) {
      expdesc cv, key;
      int pc;
      expdesc tab;

      /* ClassName.__access = {} */
      buildglobal(ls, classname, &cv);
      luaK_exp2anyregup(fs, &cv);
      codestring(&key, luaX_newstring(ls, "__access", 8));
      luaK_indexed(fs, &cv, &key);

      pc = luaK_codevABCk(fs, OP_NEWTABLE, 0, 0, 0, 0);
      luaK_code(fs, 0);
      init_exp(&tab, VNONRELOC, fs->freereg);
      luaK_reserveregs(fs, 1);
      luaK_settablesize(fs, pc, tab.u.info, 0, nfields);

      /* set fields in __access table */
      for (i = 0; i < nfields; i++) {
        expdesc t2 = tab, fk, fv;
        luaK_exp2anyregup(fs, &t2);
        codestring(&fk, field_names[i]);
        luaK_indexed(fs, &t2, &fk);
        codestring(&fv, luaX_newstring(ls, field_access[i],
                                       strlen(field_access[i])));
        luaK_storevar(fs, &t2, &fv);
      }

      luaK_storevar(fs, &cv, &tab);
      luaK_fixline(fs, line);
    }

    /* __getters and __setters are pre-created before body parsing */

    /* Call __setup_class(ClassName) */
    {
      expdesc setupfn, arg;
      int base;
      buildglobal(ls, luaX_newstring(ls, "__setup_class", 13), &setupfn);
      luaK_exp2nextreg(fs, &setupfn);
      base = fs->freereg - 1;
      buildglobal(ls, classname, &arg);
      luaK_exp2nextreg(fs, &arg);
      init_exp(&setupfn, VCALL, luaK_codeABC(fs, OP_CALL, base, 2, 1));
      luaK_fixline(fs, line);
      fs->freereg = cast_byte(base);
    }
  }

  /* Check 'implements' constraints at compile time */
  {
    int ii;
    for (ii = 0; ii < nimpl; ii++)
      check_implements(ls, classname, impl_ifaces[ii],
                       class_method_names, nclass_methods);
  }

  check_match(ls, TK_END, TK_CLASS, line);
}


/*
** matchstat -> MATCH expr { CASE expr THEN block } [CASE '_' THEN block] END
** Compiles to equivalent if/elseif/else chain.
*/
/*
** trystat -> TRY block [EXCEPT NAME THEN block] [FINALLY block] END
**
** Compiles to:
**   local __ok, __err = pcall(function() <try_body> end)
**   if not __ok then local err = __err; <except_body> end
**   <finally_body>
*/
static void trystat (LexState *ls, int line) {
  FuncState *fs = ls->fs;
  luaX_next(ls);  /* skip 'try' */

  /* Wrap try body in pcall(function() ... end) */
  /* Create closure for try body */
  expdesc pcallvar, tryclose, result;
  int base, nresults;

  /* get pcall from _ENV */
  buildglobal(ls, luaX_newstring(ls, "pcall", 5), &pcallvar);
  luaK_exp2nextreg(fs, &pcallvar);
  base = fs->freereg - 1;

  /* create function() <try_body> end as argument */
  {
    expdesc b;
    FuncState new_fs;
    BlockCnt bl;
    new_fs.f = addprototype(ls);
    new_fs.f->linedefined = line;
    open_func(ls, &new_fs, &bl);
    new_fs.classctx = NULL;
    /* parse try body until 'except', 'finally', or 'end' */
    while (ls->t.token != TK_END && ls->t.token != TK_EOS) {
      if (ls->t.token == TK_NAME) {
        const char *kw = getstr(ls->t.seminfo.ts);
        if (strcmp(kw, "except") == 0 || strcmp(kw, "finally") == 0)
          break;
      }
      statement(ls);
    }
    new_fs.f->lastlinedefined = ls->linenumber;
    codeclosure(ls, &b);
    close_func(ls);
    luaK_exp2nextreg(fs, &b);
  }

  /* call pcall(try_func): 1 arg, 2 results (ok, err) */
  init_exp(&result, VCALL, luaK_codeABC(fs, OP_CALL, base, 2, 3));
  luaK_fixline(fs, line);
  fs->freereg = cast_byte(base + 2);  /* ok in base, err in base+1 */

  /* create locals __ok, __err for the results */
  int okreg = base;
  int errreg = base + 1;
  TString *okname = luaX_newstring(ls, "(try_ok)", 8);
  TString *errname = luaX_newstring(ls, "(try_err)", 9);
  new_localvar(ls, okname);
  new_localvar(ls, errname);
  adjustlocalvars(ls, 2);

  /* parse 'except errvar then' block */
  if (ls->t.token == TK_NAME &&
      strcmp(getstr(ls->t.seminfo.ts), "except") == 0) {
    luaX_next(ls);  /* skip 'except' */

    /* except NAME then ... */
    TString *errvarname = str_checkname(ls);
    checknext(ls, TK_THEN);

    /* if not __ok then local err = __err; <except_body> end */
    /* TEST okreg, k=1 → skip JMP if falsy (error); execute JMP if truthy (ok) */
    luaK_codeABCk(fs, OP_TEST, okreg, 0, 0, 1);
    int jmp_noerr = luaK_jump(fs);

    /* error branch: create local err = __err */
    {
      BlockCnt bl;
      enterblock(fs, &bl, 0);
      new_localvar(ls, errvarname);
      luaK_codeABC(fs, OP_MOVE, fs->freereg, errreg, 0);
      luaK_reserveregs(fs, 1);
      adjustlocalvars(ls, 1);

      /* parse except body */
      while (ls->t.token != TK_END && ls->t.token != TK_EOS) {
        if (ls->t.token == TK_NAME &&
            strcmp(getstr(ls->t.seminfo.ts), "finally") == 0)
          break;
        statement(ls);
      }
      leaveblock(fs);
    }

    luaK_patchtohere(fs, jmp_noerr);
  }

  /* parse 'finally' block */
  if (ls->t.token == TK_NAME &&
      strcmp(getstr(ls->t.seminfo.ts), "finally") == 0) {
    luaX_next(ls);  /* skip 'finally' */
    /* finally body: always runs */
    while (ls->t.token != TK_END && ls->t.token != TK_EOS)
      statement(ls);
  }

  check_match(ls, TK_END, TK_TRY, line);
}


static void matchstat (LexState *ls, int line) {
  FuncState *fs = ls->fs;
  expdesc subject;
  int jmp_end_list = NO_JUMP;

  luaX_next(ls);  /* skip 'match' */

  /* store subject in a local temporary variable */
  expr(ls, &subject);
  luaK_exp2nextreg(fs, &subject);
  int reg = fs->freereg - 1;  /* register holding the subject */

  /* parse case clauses */
  while (ls->t.token == TK_NAME &&
         strcmp(getstr(ls->t.seminfo.ts), "case") == 0) {
    luaX_next(ls);  /* skip 'case' */

    /* check for default: 'case _' */
    if (ls->t.token == TK_NAME &&
        strcmp(getstr(ls->t.seminfo.ts), "_") == 0) {
      luaX_next(ls);  /* skip '_' */
      checknext(ls, TK_THEN);
      while (ls->t.token != TK_END && ls->t.token != TK_EOS)
        statement(ls);
      break;
    }

    /* parse pattern value */
    expdesc pattern;
    expr(ls, &pattern);
    luaK_exp2nextreg(fs, &pattern);

    checknext(ls, TK_THEN);

    /* generate: EQ reg, pattern_reg, 0; JMP skip */
    /* OP_EQ A B k: if ((R[A]==R[B]) ~= k) then pc++ (skip next)
       k=1: skip next if (R[A]==R[B]) is true → skip JMP → fall into body
       k=0: skip next if (R[A]==R[B]) is false → skip JMP → fall into body
       We want: if equal → execute body. if not equal → skip.
       So: k=0 → if NOT equal, skip JMP → falls through (wrong!)
       k=1 → if EQUAL, skip JMP → falls into body (right!) */
    /* OP_EQ A B k: if ((R[A]==R[B]) ~= k) then pc++ (skip next JMP)
       k=1: if NOT equal → skip JMP → go to next case (fall through)
             if EQUAL → execute JMP → jump past body... no.

       Actually need: if NOT equal → skip body.
       Use k=0: if ((eq) ~= 0) → if NOT equal → skip next.
       Next = JMP to next_case. So if NOT equal, skip JMP, fall into body (WRONG).

       Reverse: use k=1: if eq → skip JMP → fall into body.
                          if not eq → execute JMP → go to next case. CORRECT! */
    int preg = fs->freereg - 1;  /* pattern register */
    luaK_codeABCk(fs, OP_EQ, reg, preg, 0, 0);
    int jmp_skip = luaK_jump(fs);  /* executed when NOT equal → skip body */
    fs->freereg = cast_byte(reg + 1);  /* free pattern, keep subject */

    /* case body */
    while (ls->t.token != TK_END && ls->t.token != TK_EOS) {
      if (ls->t.token == TK_NAME &&
          strcmp(getstr(ls->t.seminfo.ts), "case") == 0)
        break;
      statement(ls);
    }
    fs->freereg = cast_byte(reg + 1);  /* preserve subject register */

    /* jump to end */
    luaK_concat(fs, &jmp_end_list, luaK_jump(fs));
    luaK_patchtohere(fs, jmp_skip);
    fs->freereg = cast_byte(reg + 1);  /* reset for next case */
  }

  luaK_patchtohere(fs, jmp_end_list);
  fs->freereg = cast_byte(reg);  /* free subject */

  check_match(ls, TK_END, TK_NAME, line);
}


static void statement (LexState *ls) {
  int line = ls->linenumber;  /* may be needed for error messages */
  enterlevel(ls);
  switch (ls->t.token) {
    case ';': {  /* stat -> ';' (empty statement) */
      luaX_next(ls);  /* skip ';' */
      break;
    }
    case TK_IF: {  /* stat -> ifstat */
      ifstat(ls, line);
      break;
    }
    case TK_WHILE: {  /* stat -> whilestat */
      whilestat(ls, line);
      break;
    }
    case TK_DO: {  /* stat -> DO block END */
      luaX_next(ls);  /* skip DO */
      block(ls);
      check_match(ls, TK_END, TK_DO, line);
      break;
    }
    case TK_FOR: {  /* stat -> forstat */
      forstat(ls, line);
      break;
    }
    case TK_REPEAT: {  /* stat -> repeatstat */
      repeatstat(ls, line);
      break;
    }
    case TK_FUNCTION: {  /* stat -> funcstat */
      funcstat(ls, line);
      break;
    }
    case TK_LOCAL: {  /* stat -> localstat | destructuring */
      luaX_next(ls);  /* skip LOCAL */
      if (testnext(ls, TK_FUNCTION))  /* local function? */
        localfunc(ls);
      else if (ls->t.token == '{') {
        /* table destructuring: local {a, b, c} = expr
           Compiles to: local __tmp = expr; local a=__tmp.a; local b=__tmp.b; ... */
        FuncState *fs = ls->fs;
        TString *names[MAXVARS];
        int nnames = 0;
        int i;
        luaX_next(ls);  /* skip '{' */
        do {
          if (nnames >= MAXVARS)
            luaK_semerror(ls, "too many variables in destructuring");
          names[nnames++] = str_checkname(ls);
        } while (testnext(ls, ','));
        checknext(ls, '}');
        checknext(ls, '=');
        /* create hidden temp var for the source table */
        TString *tmpname = luaX_newstring(ls, "(destructure)", 13);
        new_localvar(ls, tmpname);
        expdesc src;
        expr(ls, &src);
        adjust_assign(ls, 1, 1, &src);
        adjustlocalvars(ls, 1);  /* __tmp is now active */
        int tmpreg = getlocalvardesc(fs, fs->nactvar - 1)->vd.ridx;
        /* create locals for each field */
        for (i = 0; i < nnames; i++)
          new_localvar(ls, names[i]);
        /* extract fields: each local = __tmp.fieldname */
        for (i = 0; i < nnames; i++) {
          expdesc tab, key;
          init_exp(&tab, VNONRELOC, tmpreg);
          codestring(&key, names[i]);
          luaK_indexed(fs, &tab, &key);
          luaK_exp2nextreg(fs, &tab);
        }
        adjustlocalvars(ls, nnames);
      }
      else
        localstat(ls);
      break;
    }
    case TK_GLOBAL: {  /* stat -> globalstatfunc */
      globalstatfunc(ls, line);
      break;
    }
    case TK_CLASS: {  /* stat -> classstat */
      classstat(ls, line);
      break;
    }
    case TK_INTERFACE: {  /* stat -> interfacestat */
      interfacestat(ls, line);
      break;
    }
    case TK_ENUM: {  /* stat -> enumstat */
      enumstat(ls, line);
      break;
    }
    case TK_TRY: {  /* stat -> trystat */
      trystat(ls, line);
      break;
    }
    /* TK_MATCH removed: 'match' is contextual, handled in TK_NAME */
    case TK_DBCOLON: {  /* stat -> label */
      luaX_next(ls);  /* skip double colon */
      labelstat(ls, str_checkname(ls), line);
      break;
    }
    case TK_RETURN: {  /* stat -> retstat */
      luaX_next(ls);  /* skip RETURN */
      retstat(ls);
      break;
    }
    case TK_BREAK: {  /* stat -> breakstat */
      breakstat(ls, line);
      break;
    }
    case TK_GOTO: {  /* stat -> 'goto' NAME */
      luaX_next(ls);  /* skip 'goto' */
      gotostat(ls, line);
      break;
    }
    case TK_NAME: {
      /* 'declare class NAME ... end' - declaration-only class (no codegen) */
      if (strcmp(getstr(ls->t.seminfo.ts), "declare") == 0) {
        int lk = luaX_lookahead(ls);
        if (lk == TK_CLASS) {
          luaX_next(ls);  /* skip 'declare' */
          luaX_next(ls);  /* skip 'class' */
          TString *dname = str_checkname(ls);
          /* register class name and optionally parent */
          register_classname(ls, dname);
          TString *dparent = NULL;
          if (ls->t.token == TK_EXTENDS) {
            luaX_next(ls);
            dparent = str_checkname(ls);
          }
          register_classparent(ls, dname, dparent);
          /* Generate minimal: ClassName = ClassName or {}
             This creates a placeholder table if not already set by C */
          {
            FuncState *fs = ls->fs;
            expdesc var, val;
            int pc;
            buildglobal(ls, dname, &var);
            pc = luaK_codevABCk(fs, OP_NEWTABLE, 0, 0, 0, 0);
            luaK_code(fs, 0);
            init_exp(&val, VNONRELOC, fs->freereg);
            luaK_reserveregs(fs, 1);
            luaK_settablesize(fs, pc, val.u.info, 0, 0);
            luaK_storevar(fs, &var, &val);
            luaK_fixline(fs, line);
          }
          /* parse body: methods (no bodies) and fields */
          while (ls->t.token != TK_END && ls->t.token != TK_EOS) {
            if (ls->t.token == TK_FUNCTION) {
              luaX_next(ls);  /* skip 'function' */
              TString *mname = str_checkname(ls);
              register_classmethod_info(ls, dname, mname);
              /* skip params */
              checknext(ls, '(');
              while (ls->t.token != ')' && ls->t.token != TK_EOS) {
                if (ls->t.token == TK_NAME) {
                  luaX_next(ls);
                  optional_type_annotation(ls);
                }
                else if (ls->t.token == TK_DOTS) luaX_next(ls);
                if (ls->t.token == ',') luaX_next(ls);
              }
              checknext(ls, ')');
              optional_type_annotation(ls);
            }
            else if (ls->t.token == TK_NAME) {
              /* field or modifier: just skip */
              luaX_next(ls);
              if (ls->t.token == ':') {
                luaX_next(ls);
                parse_type(ls);
              }
            }
            else if (ls->t.token == ';') luaX_next(ls);
            else break;
          }
          check_match(ls, TK_END, TK_CLASS, line);
          break;
        }
      }
      /* check for 'type' keyword */
      if (ls->t.seminfo.ts == ls->typn) {
        int lk = luaX_lookahead(ls);
        if (lk == TK_NAME) {
          typestat(ls);
          break;
        }
      }
      /* import "module" → local module = require("module")
         import NAME from "module" → local NAME = require("module") */
      /* 'match' as contextual keyword (preserves string:match()) */
      if (ls->t.seminfo.ts == ls->matchn) {
        matchstat(ls, line);
        break;
      }
      if (ls->t.seminfo.ts == ls->importn) {
        int ilk = luaX_lookahead(ls);
        if (ilk != TK_STRING && ilk != TK_NAME)
          goto not_import;  /* not an import statement */
        FuncState *fs = ls->fs;
        luaX_next(ls);  /* skip 'import' */
        if (ls->t.token == TK_STRING) {
          /* import "modname" → local modname = require("modname") */
          TString *modstr = ls->t.seminfo.ts;
          /* extract module name from path (last component) */
          const char *ms = getstr(modstr);
          const char *lastdot = ms;
          for (const char *p = ms; *p; p++)
            if (*p == '.' || *p == '/') lastdot = p + 1;
          TString *localname = luaX_newstring(ls, lastdot, strlen(lastdot));
          luaX_next(ls);  /* skip string */
          /* generate: local localname = require("modstr") */
          new_localvar(ls, localname);
          expdesc req, arg;
          buildglobal(ls, luaX_newstring(ls, "require", 7), &req);
          luaK_exp2nextreg(fs, &req);
          int base = fs->freereg - 1;
          codestring(&arg, modstr);
          luaK_exp2nextreg(fs, &arg);
          init_exp(&req, VCALL, luaK_codeABC(fs, OP_CALL, base, 2, 2));
          luaK_fixline(fs, ls->linenumber);
          fs->freereg = cast_byte(base + 1);
          adjustlocalvars(ls, 1);
        }
        else if (ls->t.token == TK_NAME) {
          /* import NAME from "module" */
          TString *localname = ls->t.seminfo.ts;
          luaX_next(ls);  /* skip NAME */
          /* expect 'from' */
          if (ls->t.token != TK_NAME ||
              strcmp(getstr(ls->t.seminfo.ts), "from") != 0)
            luaX_syntaxerror(ls, "'from' expected in import statement");
          luaX_next(ls);  /* skip 'from' */
          if (ls->t.token != TK_STRING)
            luaX_syntaxerror(ls, "module name string expected");
          TString *modstr = ls->t.seminfo.ts;
          luaX_next(ls);  /* skip string */
          /* generate: local NAME = require("module") */
          new_localvar(ls, localname);
          expdesc req, arg;
          buildglobal(ls, luaX_newstring(ls, "require", 7), &req);
          luaK_exp2nextreg(fs, &req);
          int base = fs->freereg - 1;
          codestring(&arg, modstr);
          luaK_exp2nextreg(fs, &arg);
          init_exp(&req, VCALL, luaK_codeABC(fs, OP_CALL, base, 2, 2));
          luaK_fixline(fs, ls->linenumber);
          fs->freereg = cast_byte(base + 1);
          adjustlocalvars(ls, 1);
        }
        else {
          luaX_syntaxerror(ls, "string or name expected after 'import'");
        }
        break;
      }
      not_import:
#if defined(LUA_COMPAT_GLOBAL)
      /* compatibility code to parse global keyword when "global"
         is not reserved */
      if (ls->t.seminfo.ts == ls->glbn) {  /* current = "global"? */
        int lk = luaX_lookahead(ls);
        if (lk == '<' || lk == TK_NAME || lk == '*' || lk == TK_FUNCTION) {
          /* 'global <attrib>' or 'global name' or 'global *' or
             'global function' */
          globalstatfunc(ls, line);
          break;
        }
      }
#endif
    }
    /* FALLTHROUGH */
    default: {  /* stat -> func | assignment */
      exprstat(ls);
      break;
    }
  }
  lua_assert(ls->fs->f->maxstacksize >= ls->fs->freereg &&
             ls->fs->freereg >= luaY_nvarstack(ls->fs));
  ls->fs->freereg = luaY_nvarstack(ls->fs);  /* free registers */
  leavelevel(ls);
}

/* }====================================================================== */

/* }====================================================================== */


/*
** compiles the main function, which is a regular vararg function with an
** upvalue named LUA_ENV
*/
static void mainfunc (LexState *ls, FuncState *fs) {
  BlockCnt bl;
  Upvaldesc *env;
  open_func(ls, fs, &bl);
  setvararg(fs);  /* main function is always vararg */
  env = allocupvalue(fs);  /* ...set environment upvalue */
  env->instack = 1;
  env->idx = 0;
  env->kind = VDKREG;
  env->name = ls->envn;
  luaC_objbarrier(ls->L, fs->f, env->name);
  luaX_next(ls);  /* read first token */
  statlist(ls);  /* parse main body */
  check(ls, TK_EOS);
  close_func(ls);
}


LClosure *luaY_parser (lua_State *L, ZIO *z, Mbuffer *buff,
                       Dyndata *dyd, const char *name, int firstchar) {
  LexState lexstate;
  FuncState funcstate;
  LClosure *cl = luaF_newLclosure(L, 1);  /* create main closure */
  setclLvalue2s(L, L->top.p, cl);  /* anchor it (to avoid being collected) */
  luaD_inctop(L);
  lexstate.h = luaH_new(L);  /* create table for scanner */
  sethvalue2s(L, L->top.p, lexstate.h);  /* anchor it */
  luaD_inctop(L);
  funcstate.f = cl->p = luaF_newproto(L);
  luaC_objbarrier(L, cl, cl->p);
  funcstate.f->source = luaS_new(L, name);  /* create and anchor TString */
  luaC_objbarrier(L, funcstate.f, funcstate.f->source);
  lexstate.buff = buff;
  lexstate.dyd = dyd;
  dyd->actvar.n = dyd->gt.n = dyd->label.n = 0;
  luaX_setinput(L, &lexstate, z, funcstate.f->source, firstchar);
  mainfunc(&lexstate, &funcstate);
  lua_assert(!funcstate.prev && funcstate.nups == 1 && !lexstate.fs);
  /* all scopes should be correctly finished */
  lua_assert(dyd->actvar.n == 0 && dyd->gt.n == 0 && dyd->label.n == 0);
  /* free class name registry and field access info */
  if (lexstate.classnames != NULL)
    luaM_freearray(L, lexstate.classnames, lexstate.classnames_size);
  if (lexstate.classfields != NULL)
    luaM_freearray(L, lexstate.classfields, lexstate.classfields_size);
  if (lexstate.classmethods != NULL)
    luaM_freearray(L, lexstate.classmethods, lexstate.classmethods_size);
  if (lexstate.classparents != NULL)
    luaM_freearray(L, lexstate.classparents, lexstate.classparents_size);
  for (int ii = 0; ii < lexstate.ninterfaces; ii++) {
    if (lexstate.interfaces[ii].methods != NULL)
      luaM_freearray(L, lexstate.interfaces[ii].methods,
                     lexstate.interfaces[ii].methods_size);
  }
  if (lexstate.interfaces != NULL)
    luaM_freearray(L, lexstate.interfaces, lexstate.interfaces_size);
  for (int ii = 0; ii < lexstate.nenums; ii++) {
    if (lexstate.enums[ii].values != NULL)
      luaM_freearray(L, lexstate.enums[ii].values,
                     lexstate.enums[ii].values_size);
  }
  if (lexstate.enums != NULL)
    luaM_freearray(L, lexstate.enums, lexstate.enums_size);
  L->top.p--;  /* remove scanner's table */
  return cl;  /* closure is on the stack, too */
}

