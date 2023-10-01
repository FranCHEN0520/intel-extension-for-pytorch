import copy
import torch
from intel_extension_for_pytorch.nn.modules import IpexWoqLinear
from torch.ao.quantization import PlaceholderObserver, QConfigMapping

# The config describes how to load low precision checkpoint for weight only quantization.
# Weight shape is N by K if transposed is False otherwise K by N.
# Bias is optional. If bias is not provided in the checkpoint, we read the original model.
DEFAULT_LOWP_CHECKPOINT_CONFIG = \
    {
        "name": "default",
        "weight_key": "packed_weight",
        "scale_key": "scale",
        "zero_point_key": "packed_zp",
        "bias_key": "bias",
    }


def _is_woq_qconfig(qconfig_mapping):
    qconfig = qconfig_mapping.global_qconfig \
        if isinstance(qconfig_mapping, QConfigMapping) else qconfig_mapping
    return isinstance(qconfig.activation(), PlaceholderObserver) and \
        not qconfig.activation().is_dynamic


def _default_lowp_checkpoint_config():
    return DEFAULT_LOWP_CHECKPOINT_CONFIG


def _get_keys_from_config(checkpoint_config):
    weight_key = checkpoint_config.get('weight_key', 'weight')
    scales_key = checkpoint_config.get('scale_key', 'scale')
    zeros_key = checkpoint_config.get('zero_point_key', 'zero')
    bias_key = checkpoint_config.get('bias_key', 'bias')
    return weight_key, scales_key, zeros_key, bias_key


def _get_linear_parameters(attr_name, state_dict, checkpoint_config):
    weight_key, scales_key, zeros_key, bias_key = _get_keys_from_config(checkpoint_config)
    w_key = attr_name + '.' + weight_key
    s_key = attr_name + '.' + scales_key
    z_key = attr_name + '.' + zeros_key
    b_key = attr_name + '.' + bias_key
    # all are tensors
    qweight = state_dict.get(w_key, None)
    scales = state_dict.get(s_key, None)
    qzeros = state_dict.get(z_key, None)
    bias = state_dict.get(b_key, None)
    return qweight, scales, qzeros, bias


def _convert_woq_with_low_precision_checkpoint(
        model,
        qconfig_mapping,
        low_precision_checkpoint,
        checkpoint_config=None,
        inplace=True):
    r'''
    Method to convert fp32 model to WOQ model with checkpoint generated by GPTQ
    Args:
        model: original model
        qconfig_mapping: QConfigMapping object containing observer info, lowp mode, etc.
        low_precision_checkpoint (dict): checkpoint generated by GPTQ, etc.
        checkpoint_config (dict): custom config to load the checkpoint. Use default if None
        inplace: do conversion in-place or make a copy of original model
    Return:
        Converted model

    By default, we use the checkpoint format generated by Intel(R) Neural Compressor (INC) GPTQ.
    The default format is described by `weight_only_low_precision_checkpoint_config.json`
    Users may use custom config to override the default.
    Default format:
    - Weights and zero points in UINT4 and compressed as INT32, scales in FP16.
    - Keys are 'packed_weight', 'scale', 'packed_zp'
    '''

    assert isinstance(low_precision_checkpoint, dict), \
        'low_precision_checkpoint should be a state_dict'
    assert checkpoint_config is None or isinstance(checkpoint_config, dict), \
        'checkpoint_config should be a dict'
    if checkpoint_config is None:
        checkpoint_config = _default_lowp_checkpoint_config()

    state_dict = low_precision_checkpoint
    # Check that keys can be found in the state dict. Bias is optional.
    weight_key, scales_key, zeros_key, _ = _get_keys_from_config(checkpoint_config)
    keys_found = [False] * 3
    for k, _ in state_dict.items():
        if k.endswith('.' + weight_key):
            keys_found[0] = True
        if k.endswith('.' + scales_key):
            keys_found[1] = True
        if k.endswith('.' + zeros_key):
            keys_found[2] = True
        if all(keys_found):
            break
    assert all(keys_found), 'Error: Format of checkpoint and config do not match'

    def _convert(mod, attr_name):
        if isinstance(mod, torch.nn.Linear):
            mod.qconfig = qconfig_mapping.global_qconfig
            qweight, scales, qzeros, bias = _get_linear_parameters(
                attr_name, state_dict, checkpoint_config
            )
            if any(i is None for i in [qweight, scales, qzeros]):
                return mod
            mod_new = IpexWoqLinear.from_float_and_int4_weight(mod, qweight, scales, qzeros, bias)
            return mod_new

        mod_new = mod

        for name, child in mod.named_children():
            attr = attr_name + "." + name if attr_name != "" else name
            setattr(mod_new, name, _convert(child, attr))
        return mod_new

    if not inplace:
        model_new = copy.deepcopy(model)
    else:
        model_new = model
    return _convert(model_new, "")
