#include <stdlib.h>
#include <wchar.h>

int
main (int argc, char** argv)
{
  mbstate_t x;
  return sizeof (x) - sizeof (mbstate_t);
}
