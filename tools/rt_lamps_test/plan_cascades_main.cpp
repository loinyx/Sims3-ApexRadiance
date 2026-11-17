// The radiance cascades' kernel checks (plan_cascades_case.h) on their own: CPU only, no device (run_plan_cascades.ps1).
// check.cpp --cascades runs the same checks before it compares the cascades with the path-traced reference.
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
#include "plan_cascades_case.h"
int main() {
    (void)&PlanFieldChecks; // (run by run_plan_field.ps1)
    PlanCascadesChecks();
    std::printf("\n%d checks, %d failed\n", checks, failures);
    return failures != 0;
}
