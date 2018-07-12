#include "types.h"
#include "stat.h"
#include "user.h"

void testeCowfork()
{
  int pid;
  printf(1, "Teste do CowFork\n");

  pid = cowfork();

  if(pid==0){
    printf(1,"funcionando o processo filho\n");
  }
  else{
    printf(1,"funcionando o processo pai\n");
  }

  while (1); // Loop infinito para que ambos os processos não parem e seja possivel visualizar seus dados
}

int main(int argc, char const *argv[]) {
  testeCowfork();
  exit();
}
