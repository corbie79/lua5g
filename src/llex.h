/*
** $Id: llex.h $
** Lexical Analyzer
** See Copyright Notice in lua.h
*/

#ifndef llex_h
#define llex_h

#include <limits.h>

#include "lobject.h"
#include "lzio.h"


/*
** Single-char tokens (terminal symbols) are represented by their own
** numeric code. Other tokens start at the following value.
*/
#define FIRST_RESERVED	(UCHAR_MAX + 1)


#if !defined(LUA_ENV)
#define LUA_ENV		"_ENV"
#endif


/*
* WARNING: if you change the order of this enumeration,
* grep "ORDER RESERVED"
*/
enum RESERVED {
  /* terminal symbols denoted by reserved words */
  TK_AND = FIRST_RESERVED, TK_BREAK,
  TK_CLASS, TK_DO, TK_ELSE, TK_ELSEIF, TK_END, TK_ENUM, TK_EXTENDS,
  TK_FALSE, TK_FOR, TK_FUNCTION,
  TK_GLOBAL, TK_GOTO, TK_IF, TK_IMPLEMENTS, TK_IN, TK_INTERFACE,
  TK_LOCAL, TK_NIL, TK_NOT, TK_OR,
  TK_REPEAT, TK_RETURN, TK_THEN, TK_TRUE, TK_TRY, TK_UNTIL, TK_WHILE,
  /* other terminal symbols */
  TK_IDIV, TK_CONCAT, TK_DOTS, TK_EQ, TK_GE, TK_LE, TK_NE,
  TK_SHL, TK_SHR,
  TK_DBCOLON, TK_EOS,
  TK_FLT, TK_INT, TK_NAME, TK_STRING
};

/* number of reserved words */
#define NUM_RESERVED	(cast_int(TK_WHILE-FIRST_RESERVED + 1))


typedef union {
  lua_Number r;
  lua_Integer i;
  TString *ts;
} SemInfo;  /* semantics information */


typedef struct Token {
  int token;
  SemInfo seminfo;
} Token;


/* state of the scanner plus state of the parser when shared by all
   functions */
typedef struct LexState {
  int current;  /* current character (charint) */
  int linenumber;  /* input line counter */
  int lastline;  /* line of last token 'consumed' */
  Token t;  /* current token */
  Token lookahead;  /* look ahead token */
  struct FuncState *fs;  /* current function (parser) */
  struct lua_State *L;
  ZIO *z;  /* input stream */
  Mbuffer *buff;  /* buffer for tokens */
  Table *h;  /* to avoid collection/reuse strings */
  struct Dyndata *dyd;  /* dynamic structures used by the parser */
  TString *source;  /* current source name */
  TString *envn;  /* environment variable name */
  TString *brkn;  /* "break" name (used as a label) */
  TString *glbn;  /* "global" name (when not a reserved word) */
  TString *typn;  /* "type" name (for type alias statements) */
  TString *matchn;  /* "match" name (contextual keyword) */
  TString *enumin;  /* "enum" name (contextual keyword) */
  TString *importn; /* "import" name (contextual keyword) */
  /* class name registry for compile-time type validation */
  TString **classnames;  /* array of declared class names */
  int nclasses;  /* number of declared classes */
  int classnames_size;  /* allocated size */
  /* class field access info for compile-time access checking */
  struct ClassFieldAccess {
    TString *classname;
    TString *fieldname;
    lu_byte access;  /* 1=private, 2=protected, 3=readonly */
  } *classfields;
  int nclassfields;
  int classfields_size;
  /* class method registry for override checking */
  struct ClassMethodInfo {
    TString *classname;
    TString *methodname;
  } *classmethods;
  int nclassmethods;
  int classmethods_size;
  /* class parent registry for override checking */
  struct ClassParentInfo {
    TString *classname;
    TString *parentname;  /* NULL if no parent */
  } *classparents;
  int nclassparents;
  int classparents_size;
  /* interface registry for compile-time method checking */
  struct InterfaceInfo {
    TString *name;          /* interface name */
    TString **methods;      /* array of required method names */
    int nmethods;
    int methods_size;
  } *interfaces;
  int ninterfaces;
  int interfaces_size;
  /* enum registry */
  struct EnumInfo {
    TString *name;          /* enum name */
    TString **values;       /* array of enum value names */
    int nvalues;
    int values_size;
  } *enums;
  int nenums;
  int enums_size;
} LexState;


LUAI_FUNC void luaX_init (lua_State *L);
LUAI_FUNC void luaX_setinput (lua_State *L, LexState *ls, ZIO *z,
                              TString *source, int firstchar);
LUAI_FUNC TString *luaX_newstring (LexState *ls, const char *str, size_t l);
LUAI_FUNC void luaX_next (LexState *ls);
LUAI_FUNC int luaX_lookahead (LexState *ls);
LUAI_FUNC l_noret luaX_syntaxerror (LexState *ls, const char *s);
LUAI_FUNC const char *luaX_token2str (LexState *ls, int token);


#endif
