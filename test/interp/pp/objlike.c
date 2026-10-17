/* Object-like macros: definition, nesting, the hide set, #undef, and the
   command-line definitions the test adds (FROM_COMMAND_LINE=42, FLAG). */
#define ONE 1
#define TWO (ONE + ONE)
#define FOUR TWO * TWO
int a = FOUR;
int b = FROM_COMMAND_LINE + FLAG;
#define EMPTY
int EMPTY c EMPTY = EMPTY 3;
EMPTY
int d;
/* A macro naming itself stops; two naming each other stop after one round. */
#define self self + 1
#define ping pong
#define pong ping
int e = self, f = ping, g = pong;
/* Redefining identically is fine; #undef and a new definition too. */
#define ONE 1
#undef ONE
#define ONE 100
int h = ONE;
/* A body spanning lines, and tokens with no space between them stay so. */
#define LONG a + \
    b + \
    c
int i = LONG;
#define GLUED a+b
int j = GLUED, k = (GLUED);
/* Predefined. */
int l = __STDC__, m = __STDC_VERSION__, n = __GOOSE_INTERP__;
