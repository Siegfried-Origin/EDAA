// Event registration of each module.

#pragma once

namespace edaa
{
	void register_camera_events();
	void unregister_camera_events();

	void register_frame_events();
	void unregister_frame_events();

	void register_pass_events();
	void unregister_pass_events();
}
