export interface DemoAudio { monitor: string; stream: string }

// Only set these after public release / aligned final audio verification.
export const publication: { audio: DemoAudio | null; repository: string } = {
  audio: null,
  repository: 'https://github.com/LiuTouo/RoudaMix',
};
