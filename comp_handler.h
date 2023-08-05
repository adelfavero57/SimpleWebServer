#ifndef COMP_HANDLER_H
#define COMP_HANDLER_H


struct code_wrapper {
  uint8_t code_len;
  uint32_t code;
};

struct code_wrapper* read_comp_dict();

#endif