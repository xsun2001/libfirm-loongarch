typedef struct {
    int x, y;
} point_t;
point_t p = { 1, 2 };
int main() { return p.x + p.y; }