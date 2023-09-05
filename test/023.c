int main() {
    int a = 1, *p = &a;
    int b = *p ? 10 : 20;
    return 2 * b;
}