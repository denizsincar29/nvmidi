#include <stdio.h>
#include <windows.h>
typedef void* (*plugin_main_t)(void*);
int main(int argc, char** argv){
  if(argc<3){ printf("usage: plug_api <dll> <blob>\n"); return 2; }
  HMODULE h = LoadLibraryA(argv[1]);
  if(!h){ printf("LoadLibrary failed %lu\n", GetLastError()); return 3; }
  plugin_main_t pm = (plugin_main_t)GetProcAddress(h,"plugin_main");
  printf("plugin_main=%p\n", (void*)pm);
  FILE* f=fopen(argv[2],"rb");
  if(!f){ printf("noblob\n"); return 4; }
  static unsigned char buf[8192]; size_t n=fread(buf,1,sizeof buf,f); fclose(f);
  printf("blob %zu bytes\n", n);
  if(pm){ int r = 0; printf("calling plugin_main(blob)\n"); fflush(stdout);
          r = (int)(long long)pm(buf); printf("plugin_main returned %d\n", r); }
  return 0;
}
