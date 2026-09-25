#include "test_framework.h"

int main()
{
    std::printf("running %zu tests\n", ::ob_test::tests().size());
    return ::ob_test::run_all();
}
