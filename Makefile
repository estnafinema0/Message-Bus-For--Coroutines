GCC_FLAGS = -Wextra -Werror -Wall -Wno-gnu-folding-constant -g

all:
	gcc $(GCC_FLAGS)  utils/heap_help/heap_help.c \
	libcoro.c corobus.c test.c utils/unit.c \
	-I ../utils -o test
	
# For automatic testing systems to be able to just build whatever was submitted
# test_glob:
# 	gcc $(GCC_FLAGS) *.c ../utils/unit.c -I ../utils -o test

run: all
	./test

clean:
	rm -f test *.o 