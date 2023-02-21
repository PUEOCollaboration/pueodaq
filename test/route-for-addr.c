#include "pueodaq-net.h" 

int main(int nargs, char ** args) 
{
   for (int i = 1; i < nargs; i++) 
   {
     printf("Route for %s:\n\t", args[i]); 
     route_entry_print(get_route_for_addr(args[i],NULL),stdout); 
   }

   return 0; 
}
