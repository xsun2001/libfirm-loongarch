int main() {
    int a = 0;
    int* p = &a;
    *p = 100;
    return a;
}