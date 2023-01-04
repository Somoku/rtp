#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdbool.h>

int main(int argc, char** argv){
    printf("%s\n", argv[1]);
    printf("%s\n", argv[2]);
    FILE* fp1 = fopen(argv[1], "rb");
    FILE* fp2 = fopen(argv[2], "rb");
    char f1_buffer[65536];
    char f2_buffer[65536];
    while (1)
    {
        int size1 = fread((void*)f1_buffer, sizeof(char), 65536, fp1);
        int size2 = fread((void*)f2_buffer, sizeof(char), 65536, fp2);
        if (size1 != size2){
            printf("size1 = %d\n", size1);
            printf("size2 = %d\n", size2);
            printf("Size error.\n");
            return -1;
        }
        /*
        for (int i = 0 ; i < size1; i++)
        {
            if (f1_buffer[i] != f2_buffer[i]){
                printf("Content error in %d.\n", i);
                return -1;
            }
        }
        */
        if (feof(fp1))
        {
            if (!feof(fp2)){
                printf("Length error.\n");
                return -1;
            }
            else{
                printf("Success.\n");
                return 1;
            }
        } 
    }
    return 0;
}