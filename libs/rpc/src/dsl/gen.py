import sys

def gen():
    for i in range(10):
        yield i
        print(i)


if __name__ == "__main__":
    sys.argv[0]
    for v in gen():
        pass