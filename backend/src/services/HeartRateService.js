const heartRateRepository = require('../repositories/HeartRateRepository');

class HeartRateService {
  async registerReading(dto) {
    if (typeof dto.bpm !== 'number') {
      throw new Error('O campo bpm deve ser um número.');
    }

    return await heartRateRepository.create(dto);
  }

  async getDailyReport() {
    const startOfDay = new Date();
    startOfDay.setHours(0, 0, 0, 0);

    const endOfDay = new Date();
    endOfDay.setHours(23, 59, 59, 999);

    const logs = await heartRateRepository.findManyByDateRange(startOfDay, endOfDay);

    if (logs.length === 0) {
      return { message: 'Nenhuma leitura registrada hoje.', summary: null };
    }

    const totalReadings = logs.length;
    const sumBpm = logs.reduce((acc, log) => acc + log.bpm, 0);
    const avgBpm = Math.round(sumBpm / totalReadings);
    const maxBpm = Math.max(...logs.map(log => log.bpm));
    const minBpm = Math.min(...logs.map(log => log.bpm));
    const alertCount = logs.filter(log => log.isAlert).length;

    return {
      date: new Date().toISOString().split('T')[0],
      summary: {
        totalReadings,
        avgBpm,
        maxBpm,
        minBpm,
        alertCount
      },
      logs
    };
  }
}

module.exports = new HeartRateService();