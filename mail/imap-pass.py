from sys import argv
from subprocess import check_output

def get_pass(arg):
    return check_output(['pass', arg]).split()[0]

if __name__ == '__main__':
    print(get_pass(argv[1]))
