// GTA V's camera and heading conventions, turned into the plain vectors the link speaks.
#pragma once

#include <cmath>

namespace gta
{
	constexpr float kDegrees = 3.14159265358979f / 180.0f;

	struct Basis
	{
		float right[3];
		float forward[3];
		float up[3];
	};

	/// The camera's axes in the world for a rotation as GET_FINAL_RENDERED_CAM_ROT(2) gives it: x pitch, y roll and z yaw in
	/// degrees, applied as Rz(yaw) Rx(pitch) Ry(roll) to a camera whose right is +X, forward +Y and up +Z. Yaw 0 looks along
	/// +Y (north) and turns counter-clockwise seen from above; a positive pitch looks up.
	inline Basis basis_from_rotation(float pitch, float roll, float yaw)
	{
		const float cp = std::cos(pitch * kDegrees), sp = std::sin(pitch * kDegrees);
		const float cr = std::cos(roll * kDegrees), sr = std::sin(roll * kDegrees);
		const float cy = std::cos(yaw * kDegrees), sy = std::sin(yaw * kDegrees);
		// Rx(pitch) Ry(roll) applied to each axis, then Rz(yaw)
		const float local[3][3] = {
			{cr, sp * sr, -cp * sr},  // right
			{0.0f, cp, sp},           // forward
			{sr, -sp * cr, cp * cr},  // up
		};
		Basis basis;
		float *out[3] = {basis.right, basis.forward, basis.up};
		for (int i = 0; i < 3; ++i)
		{
			out[i][0] = cy * local[i][0] - sy * local[i][1];
			out[i][1] = sy * local[i][0] + cy * local[i][1];
			out[i][2] = local[i][2];
		}
		return basis;
	}
}
