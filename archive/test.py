def angle_value_at_index(center_angle, variation_angle, num_values, index):
    step_size = variation_angle / (num_values - 1)
    start_angle = center_angle - variation_angle / 2
    return start_angle + index * step_size

# Example usage:
center_angle = 10
variation_angle = 10
num_values = 11

for i in range(num_values):
    value = angle_value_at_index(center_angle, variation_angle, num_values, i)
    print(f"Value at index {i}: {value}")
