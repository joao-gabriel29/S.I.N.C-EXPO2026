const prisma = require('../config/prisma');

class HeartRateRepository {
  async create(data) {
    return await prisma.heartRateLog.create({
      data: {
        bpm: data.bpm,
        isAlert: data.isAlert
      }
    });
  }

  async findManyByDateRange(startDate, endDate) {
    return await prisma.heartRateLog.findMany({
      where: {
        createdAt: {
          gte: startDate,
          lte: endDate
        }
      },
      orderBy: { createdAt: 'desc' }
    });
  }
}

module.exports = new HeartRateRepository();