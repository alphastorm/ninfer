void rdc_left(int*);
void rdc_right(int*);

// Force both archive members into the host link without requiring GPU execution.
int main(int argc, char**) {
    if (argc == 42) {
        rdc_left(nullptr);
        rdc_right(nullptr);
    }
    return 0;
}
