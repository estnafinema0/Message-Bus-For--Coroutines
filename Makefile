GCC_FLAGS = -Wextra -Werror -Wall -Wno-gnu-folding-constant -g

all:
	gcc $(GCC_FLAGS) libcoro.c corobus.c test.c utils/unit.c \
		-I ../utils -o test

run: all
	valgrind --leak-check=full ./test
	
# For automatic testing systems to be able to just build whatever was submitted
test_glob:
	gcc $(GCC_FLAGS) *.c ../utils/unit.c -I ../utils -o test

clean:
	rm -f test *.o 