# Author: Tom Furnival
# License: GPLv3

from hyperspy.signals import BaseSignal

from .pguresvt import SVT


class HSPYSVT(SVT):
    """HyperSpy wrapper for SVT.

    Allows arbitrary HyperSpy signals to be passed for SVT denoising.

    """

    def __init__(self, *args, **kwargs):
        super(HSPYSVT, self).__init__(*args, **kwargs)
        self._signal_type = None
        self._X = None

    def _prepare_to_denoise(self, signal):
        """Convert the signal to correctly-aligned array.

        Represents `signal` as a spectrum (three dimensions; relevant axis last)
        then returns the data, properly aligned.

        Parameters
        ----------
        signal : hyperspy.signals.BaseSignal
            The HyperSpy signal to denoise. Can be of arbitrary type/shape.

        Returns
        -------
        None

        """
        sig_dim = signal.axes_manager.signal_dimension

        if sig_dim == 1:
            self._X = signal._data_aligned_with_axes
            self._signal_type = "spectrum"

        elif sig_dim == 2:
            signal.unfold_navigation_space()
            signal_3d = signal.as_signal1D(spectral_axis=0)
            self._X = signal_3d._data_aligned_with_axes
            signal.fold()
            self._signal_type = "image"

        else:
            raise NotImplementedError(
                f"Expected 1D or 2D signal - got dimension {sig_dim}"
            )

    def _denoised_data_to_signal(self):
        """Converts denoised data back to a HyperSpy signal."""
        signal = BaseSignal(self.Y_)

        if self._signal_type == "spectrum":
            return signal.as_signal1D(2)

        if self._signal_type == "image":
            return signal.as_signal2D((1, 2))

    def denoise(self, signal):
        """Denoises an arbitrary HyperSpy signal.

        Parameters
        ----------
        signal : hyperspy.signals.Signal
            The HyperSpy signal to denoise; can be of arbitrary type/shape.

        Returns
        -------
        hyperspy.signals.Spectrum
            Denoised data as a spectrum.

        """
        self._prepare_to_denoise(signal)

        super(HSPYSVT, self).denoise(self._X)

        return self._denoised_data_to_signal()
