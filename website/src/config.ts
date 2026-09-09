export interface Release { version: string; url: string }
export interface DemoAudio { monitor: string; stream: string }

// Only set these after public release / aligned final audio verification.
export const publication: { release: Release | null; audio: DemoAudio | null; repository: string } = {
  release: null,
  audio: null,
  repository: 'https://github.com/LiuTouo/RoudaMix',
};
