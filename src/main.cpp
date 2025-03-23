#include <iostream>

#include "misc/util/abc_global.h"
#include "base/main/main.h"
#include "base/main/mainInt.h"

extern "C"
{
   int Abc_RealMain(int argc, char *argv[]);
}

int Hello_Command( Abc_Frame_t * pAbc, int argc, char ** argv )
{
   std::cout << "Hello world!" << std::endl;
   return 0;
}

struct CmdRegister
{
   CmdRegister()
   {
      Cmd_CommandAdd(Abc_FrameGetGlobalFrame(), "Hello", "@hello", Hello_Command, 0);
   }
} regiter;

int main(int argc, char *argv[])
{
   return ABC_NAMESPACE_PREFIX Abc_RealMain(argc, argv);
}