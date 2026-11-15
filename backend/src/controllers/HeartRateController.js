const heartRateService = require('../services/HeartRateService');

class HeartRateController {
  async create(req, res) {
    try {
      const { bpm, isAlert } = req.body;
      const newLog = await heartRateService.registerReading({ bpm, isAlert });
      
      return res.status(201).json({
        message: 'Leitura registrada com sucesso!',
        data: newLog
      });
    } catch (error) {
      return res.status(400).json({ error: error.message });
    }
  }

  async getReport(req, res) {
    try {
      const report = await heartRateService.getDailyReport();
      return res.status(200).json(report);
    } catch (error) {
      return res.status(500).json({ error: 'Erro ao gerar relatório diário.' });
    }
  }
}

module.exports = new HeartRateController();