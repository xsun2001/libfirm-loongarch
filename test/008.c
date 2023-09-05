int main() {
    int  a = 0, b = 0;
    int *pa = &a, *pb = &b;
    *pa = 10;
    *pb = 20;
    return a + b;
}