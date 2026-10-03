// Host check for main/ui/button_logic.h. Run: test/run_host_tests.sh
#include <assert.h>
#include <stdio.h>
#include "../main/ui/button_logic.h"

static button_state_t st;
static int toggles, setups, statuses;
static void run(int k1, int k2, int k3, int ms)
{
    for (int i = 0; i < ms / 50; i++) {
        int a = button_step(&st, k1, k2, k3, 50, 10000);
        toggles += !!(a & BTN_TOGGLE_PAUSE);
        setups += !!(a & BTN_OPEN_SETUP);
        statuses += !!(a & BTN_SHOW_STATUS);
    }
}

int main(void)
{
    run(1,0,0,200); run(0,0,0,200);                        assert(toggles == 1 && setups == 0);  // KEY1 short press
    run(1,0,0,100); run(1,0,1,10500); run(0,0,1,150); run(0,0,0,200);
                                                           assert(toggles == 1 && setups == 1);  // combo, KEY3 released last
    run(1,0,1,10500); run(1,0,0,150); run(0,0,0,200);      assert(toggles == 1 && setups == 2);  // combo, KEY1 released last (field bug)
    run(1,0,0,3000); run(0,0,0,200);                       assert(toggles == 1);                 // long KEY1 press: nothing
    run(1,0,1,2000); run(0,0,0,200);                       assert(setups == 2 && toggles == 1);  // short combo: nothing
    run(0,1,0,200); run(0,0,0,100);                        assert(statuses == 1);                // KEY2 short press
    puts("PASS: button state machine");
    return 0;
}
