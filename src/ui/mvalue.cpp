#include "mvalue.h"
#include <math.h>

using namespace spotykach;

MValue::MValue():
_in_value     { 0.1f },
_value        { 0.f },
_is_active    { false },
_is_tracking  { false }
{
  static int id = 1;
  _id = id++;
};

float MValue::process(const float value, const bool active, int* id, const float thresh) {
  _in_value = value;
  if (!_set_active(active)) return _value;
  *id = _id;
  if (!_is_tracking && abs(value - _value) > thresh) return _value;
  _is_tracking = true;
  _apply = true;
  _value = value;
  return _value;
};

bool MValue::_set_active(const bool active) {
  if (_is_active && !active) {
    _is_tracking = false;
  }
  _is_active = active;
  return active;
}
