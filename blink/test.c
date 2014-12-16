#include <stdint.h>
#define RXCIE 7
#define TXCIE 6
#define UDRIE 5

int main(int argc, char * argv[]){
    uint8_t led  = 0x02;
    printf("0x%x\n", (uint8_t)~led);
}
