void rdc_left(int*);
void rdc_right(int*);
void rdc_internal(int*);

// Force every archive member into the host link without requiring GPU execution.
int main(int argc, char**) {
    if (argc == 42) {
        rdc_left(nullptr);
        rdc_right(nullptr);
        rdc_internal(nullptr);
    }
    return 0;
}
