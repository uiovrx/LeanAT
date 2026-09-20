#include <systemc>
int leanat_descriptor_main(int, char **);
struct DescriptorRun : sc_core::sc_module {
  int argc;
  char **argv;
  int result{2};
  sc_core::sc_event wake;
  SC_HAS_PROCESS(DescriptorRun);
  DescriptorRun(sc_core::sc_module_name n, int c, char **v) : sc_module(n), argc(c), argv(v) {
    SC_METHOD(run);
    sensitive << wake;
    dont_initialize();
    wake.notify(sc_core::sc_time::from_value(1));
  }
  void run() {
    result = leanat_descriptor_main(argc, argv);
    sc_core::sc_stop();
  }
};
int sc_main(int argc, char **argv) {
  sc_core::sc_report_handler::set_actions(sc_core::SC_INFO, sc_core::SC_DO_NOTHING);
  DescriptorRun run("descriptor_run", argc, argv);
  sc_core::sc_start();
  return run.result;
}
