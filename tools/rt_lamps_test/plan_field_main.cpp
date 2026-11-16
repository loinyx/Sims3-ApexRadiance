// The wall-plan visibility field's checks (plan_field_check.h) on their own: CPU only, no device (run_plan_field.ps1).
// check.cpp runs the same checks with the rest of the lamps test.
#include <cstdio>
#include <string>
static int checks = 0, failures = 0;
static void Check(bool ok, const std::string& text) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL: %s\n", text.c_str());
    }
}
#include "plan_field_check.h"
int main() {
    PlanFieldChecks();
    std::printf("\n%d checks, %d failed\n", checks, failures);
    return failures != 0;
}
