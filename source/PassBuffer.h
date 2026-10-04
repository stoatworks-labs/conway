#pragma once

#include <FFGLSDK.h>

namespace conway
{
/**
    An off-screen buffer: a colour texture and a framebuffer, and nothing else.

    stencil's PassBuffer, not an `ffglex::FFGLFBO`, because the automaton's
    state is R32UI and `FFGLFBO::GenerateColorTexture()` allocates every
    texture with `GL_RGBA, GL_FLOAT` as the upload format (SDK b1afaf9). For
    an integer internal format that is GL_INVALID_OPERATION even with no data,
    so the texture never exists and the framebuffer is incomplete. Here the
    upload format follows the internal format. It also carries no depth
    buffer, which an FFGLFBO always does and nothing here reads.

    What it keeps from the fleet's PassBuffer: it reallocates only when the
    size or format changes; a new buffer is cleared (undefined memory in a
    buffer that feeds back into itself is noise that never washes out); and
    `Destroy()` really deletes the colour texture, which `FFGLFBO::Release()`
    leaks.

    Nothing here uses an `ffglex::Scoped*` binding for its own work, because
    those clear to 0 on exit instead of restoring: allocating a buffer inside
    one would silently unbind whatever the caller had bound. It saves and
    restores the texture binding, the framebuffer and the viewport by hand --
    and the plugin still allocates everything before it binds anything.
*/
class PassBuffer
{
public:
	/// Allocate at this size and internal format, reusing the existing buffer
	/// if it already matches. Never filtered: every read is a texelFetch.
	/// Returns false if the framebuffer is incomplete.
	bool Ensure( GLsizei width, GLsizei height, GLint internalFormat );

	/// Clear to zero.
	void Clear();

	/// Replace the whole texture with `pixels`, rows bottom-up, in the upload
	/// format that goes with the internal format.
	void Upload( const void* pixels );

	void Destroy();

	/// Hand everything over to `other` (which is destroyed first), leaving
	/// this empty. How the state survives a re-grid: the old pair moves out
	/// of the way, the new pair is allocated, the old is copied in and freed.
	void MoveTo( PassBuffer& other );

	GLuint TextureID() const
	{
		return texture;
	}
	GLuint FramebufferID() const
	{
		return framebuffer;
	}
	GLsizei Width() const
	{
		return width;
	}
	GLsizei Height() const
	{
		return height;
	}
	bool IsValid() const
	{
		return framebuffer != 0;
	}

	/// Bind this buffer's framebuffer and set the viewport to cover it.
	void BindForDrawing() const;

private:
	GLuint texture     = 0;
	GLuint framebuffer = 0;
	GLsizei width      = 0;
	GLsizei height     = 0;
	GLint format       = 0;
};

} // namespace conway
